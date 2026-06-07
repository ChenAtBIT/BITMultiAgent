
- [A2A 协议详细说明](#a2a-协议详细说明)
  - [概述](#概述)
  - [协议架构](#协议架构)
  - [核心概念](#核心概念)
    - [Agent Card](#agent-card)
    - [Agent Message](#agent-message)
    - [Agent Task](#agent-task)
    - [Task Status](#task-status)
  - [状态机](#状态机)
    - [状态转换规则](#状态转换规则)
  - [API 方法（请求与响应）](#api-方法请求与响应)
    - [message/send](#messagesend)
    - [message/stream](#messagestream)
    - [task/get](#taskget)
    - [task/cancel](#taskcancel)
  - [适配层](#适配层)
    - [A2AAdapter](#a2aadapter)
    - [RequestAdapter](#requestadapter)
    - [ResponseAdapter](#responseadapter)
    - [ErrorMapper](#errormapper)
  - [任务管理](#任务管理)
    - [TaskManagerWrapper](#taskmanagerwrapper)
    - [存储后端](#存储后端)
  - [Agent 注册中心](#agent-注册中心)
    - [RegistryClient](#registryclient)
    - [心跳机制](#心跳机制)
  - [Agent 路由](#agent-路由)
    - [AgentRouter](#agentrouter)
    - [路由策略](#路由策略)
  - [配置](#配置)
    - [A2AConfig](#a2aconfig)
  - [错误处理](#错误处理)
  - [监控指标](#监控指标)
  - [最佳实践](#最佳实践)
  - [项目实现 A2A 的请求与响应](#项目实现-a2a-的请求与响应)
- [数据层：Redis 存储](#数据层redis-存储)
  - [键与值](#键与值)
- [分层上下文记忆管理器](#分层上下文记忆管理器)
  - [设计目标](#设计目标)
  - [核心模块](#核心模块)
  - [记忆分层](#记忆分层)
  - [运行流程](#运行流程)
  - [长期记忆文件](#长期记忆文件)
  - [LLM 摘要与长期提取](#llm-摘要与长期提取)
  - [接入位置](#接入位置)
  - [失败降级](#失败降级)
  - [测试与编译](#测试与编译)




## A2A 协议详细说明
### 概述

A2A (Agent-to-Agent) 协议是一种用于 AI Agent 之间通信的标准协议。本框架实现了 A2A 协议的核心功能，支持多 Agent 协作场景。

### 协议架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         A2A Protocol                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐    HTTP/JSON-RPC    ┌─────────────┐           │
│  │   Client    │ ?─────────────────? │   Server    │           │
│  │  (A2AClient)│                     │(TaskManager)│           │
│  └─────────────┘                     └─────────────┘           │
│                                                                 │
│  消息格式:                                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ {                                                        │   │
│  │   "jsonrpc": "2.0",                                     │   │
│  │   "method": "message/send",                             │   │
│  │   "params": { ... },                                    │   │
│  │   "id": "request-id"                                    │   │
│  │ }                                                        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 核心概念

#### Agent Card

Agent 的元数据描述，包含 Agent 的能力和配置信息。

```cpp
struct AgentCard {
    std::string name;           // Agent 名称
    std::string description;    // Agent 描述
    std::string url;            // Agent URL
    std::string version;        // 版本号
    std::vector<std::string> skills;  // 技能列表
    
    // 可选字段
    std::string provider;       // 提供者
    std::string documentation_url;  // 文档 URL
    AuthenticationInfo authentication;  // 认证信息
};
```

#### Agent Message

Agent 之间传递的消息。

```cpp
struct AgentMessage {
    std::string role;           // "user" 或 "agent"
    std::vector<MessagePart> parts;  // 消息部分
    std::string context_id;     // 上下文 ID
    std::string message_id;     // 消息 ID
    std::string timestamp;      // 时间戳
};

struct MessagePart {
    std::string type;           // "text", "image", "file" 等
    std::string content;        // 内容
    std::string mime_type;      // MIME 类型
};
```

#### Agent Task

任务表示一次完整的交互。

```cpp
struct AgentTask {
    std::string id;             // 任务 ID
    std::string context_id;     // 上下文 ID
    TaskStatus status;          // 任务状态
    std::vector<AgentMessage> messages;  // 消息历史
    std::vector<Artifact> artifacts;     // 产出物
    std::string created_at;     // 创建时间
    std::string updated_at;     // 更新时间
};
```

#### Task Status

任务状态枚举。

```cpp
enum class TaskState {
    SUBMITTED,   // 已提交
    RUNNING,     // 运行中
    COMPLETED,   // 已完成
    FAILED,      // 失败
    CANCELED     // 已取消
};

struct TaskStatus {
    TaskState state;
    std::string message;        // 状态消息
    int progress;               // 进度 (0-100)
};
```

### 状态机

```
                    ┌──────────────┐
                    │  SUBMITTED   │
                    └──────┬───────┘
                           │
                           ▼
                    ┌──────────────┐
                    │   RUNNING    │
                    └──────┬───────┘
                           │
           ┌───────────────┼───────────────┐
           ▼               ▼               ▼
    ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
    │  COMPLETED   │ │    FAILED    │ │   CANCELED   │
    └──────────────┘ └──────────────┘ └──────────────┘
```

#### 状态转换规则

| 当前状态 | 允许转换到 |
|----------|------------|
| SUBMITTED | RUNNING, CANCELED |
| RUNNING | COMPLETED, FAILED, CANCELED |
| COMPLETED | (终态) |
| FAILED | (终态) |
| CANCELED | (终态) |

### API 方法（请求与响应）

#### message/send

发送消息给 Agent。

**请求:**
```json
{
    "jsonrpc": "2.0",
    "method": "message/send",
    "params": {
        "message": {
            "role": "user",
            "parts": [
                {
                    "type": "text",
                    "content": "计算 123 + 456"
                }
            ]
        },
        "context_id": "ctx-001"
    },
    "id": "req-001"
}
```

**响应:**
```json
{
    "jsonrpc": "2.0",
    "result": {
        "task": {
            "id": "task-001",
            "context_id": "ctx-001",
            "status": {
                "state": "completed",
                "message": "计算完成"
            },
            "messages": [
                {
                    "role": "agent",
                    "parts": [
                        {
                            "type": "text",
                            "content": "123 + 456 = 579"
                        }
                    ]
                }
            ]
        }
    },
    "id": "req-001"
}
```

#### message/stream

流式发送消息。

**请求:** 同 message/send

**响应:** Server-Sent Events (SSE)
```
event: thinking
data: {"content": "正在分析问题..."}

event: content
data: {"content": "123 + 456 = "}

event: content
data: {"content": "579"}

event: done
data: {"task_id": "task-001"}
```

#### task/get

获取任务状态。

**请求:**
```json
{
    "jsonrpc": "2.0",
    "method": "task/get",
    "params": {
        "task_id": "task-001"
    },
    "id": "req-002"
}
```

**响应:**
```json
{
    "jsonrpc": "2.0",
    "result": {
        "task": {
            "id": "task-001",
            "status": {
                "state": "completed"
            }
        }
    },
    "id": "req-002"
}
```

#### task/cancel

取消任务。

**请求:**
```json
{
    "jsonrpc": "2.0",
    "method": "task/cancel",
    "params": {
        "task_id": "task-001"
    },
    "id": "req-003"
}
```

### 适配层

#### A2AAdapter

将 gRPC 请求转换为 A2A 协议。

```cpp
#include "agent_rpc/a2a_adapter/a2a_adapter.h"

A2AConfig config;
config.orchestrator_url = "http://localhost:5000";
config.request_timeout_seconds = 30;

A2AAdapter adapter(config);
adapter.initialize();

// 同步查询
AIQueryRequest request;
request.set_question("计算 123 + 456");

AIQueryResponse response = adapter.processQuery(request);

// 流式查询
adapter.processQueryStreaming(request, [](const AIStreamEvent& event) {
    std::cout << event.content() << std::flush;
});
```

#### RequestAdapter

请求转换器。

```cpp
#include "agent_rpc/a2a_adapter/request_adapter.h"

RequestAdapter adapter;

// 转换 RPC 请求为 A2A 消息
AIQueryRequest rpc_request;
rpc_request.set_question("Hello");
rpc_request.set_context_id("ctx-001");

a2a::MessageSendParams a2a_params = adapter.convertToA2A(rpc_request);
```

#### ResponseAdapter

响应转换器。

```cpp
#include "agent_rpc/a2a_adapter/response_adapter.h"

ResponseAdapter adapter;

// 转换 A2A 响应为 RPC 响应
a2a::AgentTask a2a_task;
// ... 填充 a2a_task

AIQueryResponse rpc_response = adapter.convertFromA2A(a2a_task);
```

#### ErrorMapper

错误码映射。

```cpp
#include "agent_rpc/a2a_adapter/error_mapper.h"

ErrorMapper mapper;

// A2A 错误码 -> gRPC 状态码
a2a::ErrorCode a2a_error = a2a::ErrorCode::TASK_NOT_FOUND;
grpc::StatusCode grpc_status = mapper.mapToGrpc(a2a_error);
// 结果: grpc::StatusCode::NOT_FOUND
```

| A2A 错误码 | gRPC 状态码 |
|------------|-------------|
| INVALID_REQUEST | INVALID_ARGUMENT |
| METHOD_NOT_FOUND | UNIMPLEMENTED |
| TASK_NOT_FOUND | NOT_FOUND |
| AGENT_UNAVAILABLE | UNAVAILABLE |
| TIMEOUT | DEADLINE_EXCEEDED |
| INTERNAL_ERROR | INTERNAL |

### 任务管理

#### TaskManagerWrapper

任务管理器封装。

```cpp
#include "agent_rpc/a2a_adapter/task_manager_wrapper.h"

TaskManagerWrapper manager;

// 创建任务
std::string task_id = manager.createTask("ctx-001");

// 更新状态
manager.updateStatus(task_id, TaskState::RUNNING, "处理中");

// 添加消息
AgentMessage message;
message.role = "agent";
message.parts.push_back({"text", "Hello"});
manager.addMessage(task_id, message);

// 获取任务
auto task = manager.getTask(task_id);

// 查询历史
auto history = manager.getHistory("ctx-001");
```

#### 存储后端

支持多种存储后端：

| 后端 | 描述 | 适用场景 |
|------|------|----------|
| MemoryTaskStore | 内存存储 | 开发测试 |
| RedisTaskStore | Redis 存储 | 生产环境 |

```cpp
// 使用内存存储
A2AConfig config;
config.enable_redis_store = false;

// 使用 Redis 存储
config.enable_redis_store = true;
config.redis_url = "localhost:6379";
```

### Agent 注册中心

#### RegistryClient

注册中心客户端。

```cpp
#include "agent_rpc/orchestrator/registry_client.h"

RegistryClient client("http://localhost:8500");
client.connect();

// 注册 Agent
AgentCard card;
card.name = "math-agent";
card.skills = {"math", "calculation"};
client.registerAgent(card);

// 发现 Agent
auto agents = client.discoverAgents("math");

// 心跳
client.startHeartbeatLoop(30); // 30秒间隔
```

#### 心跳机制

```
┌─────────────┐                    ┌─────────────┐
│    Agent    │                    │  Registry   │
└──────┬──────┘                    └──────┬──────┘
       │                                  │
       │  ──── register ────────────────? │
       │                                  │
       │  ?─── registered ────────────── │
       │                                  │
       │  ──── heartbeat ───────────────? │ (每30秒)
       │                                  │
       │  ?─── ack ─────────────────────  │
       │                                  │
       │  ──── heartbeat ───────────────? │
       │                                  │
       │  ?─── ack ─────────────────────  │
       │                                  │
       │         ...                      │
       │                                  │
       │  (超时未收到心跳)                  │
       │                                  │
       │                    Agent 标记为不健康
       │                                  │
```

### Agent 路由

#### AgentRouter

Agent 路由器。

```cpp
#include "agent_rpc/orchestrator/agent_router.h"

AgentRouter router;
router.initialize(RoutingStrategy::SKILL_MATCH);

// 更新 Agent 列表
std::vector<AgentInfo> agents = registry.discoverAgents();
router.updateAgentList(agents);

// 选择 Agent
auto selected = router.selectAgent("计算 123 + 456", {"math"});
if (selected.has_value()) {
    std::cout << "选择: " << selected->name << std::endl;
}

// 健康状态管理
router.markAgentUnhealthy("agent-001");
router.markAgentHealthy("agent-001");
```

#### 路由策略

| 策略 | 描述 |
|------|------|
| ROUND_ROBIN | 轮询选择 |
| RANDOM | 随机选择 |
| SKILL_MATCH | 技能匹配 |
| LEAST_LOAD | 最少负载 |

### 配置

#### A2AConfig

```cpp
struct A2AConfig {
    // Orchestrator 配置
    std::string orchestrator_url = "http://localhost:5000";
    int orchestrator_port = 5000;
    
    // Registry 配置
    std::string registry_url = "http://localhost:8500";
    
    // 存储配置
    bool enable_redis_store = false;
    std::string redis_url = "localhost:6379";
    
    // 超时配置
    int request_timeout_seconds = 30;
    
    // 历史配置
    int history_length = 10;
};
```

### 错误处理

```cpp
try {
    auto response = adapter.processQuery(request);
} catch (const a2a::A2AException& e) {
    switch (e.error_code()) {
        case a2a::ErrorCode::AGENT_UNAVAILABLE:
            // 重试或降级
            break;
        case a2a::ErrorCode::TIMEOUT:
            // 超时处理
            break;
        default:
            // 其他错误
            break;
    }
}
```

### 监控指标

| 指标 | 描述 |
|------|------|
| a2a_requests_total | A2A 请求总数 |
| a2a_request_latency_ms | 请求延迟 |
| a2a_errors_total | 错误总数 |
| a2a_tasks_active | 活跃任务数 |
| a2a_agents_healthy | 健康 Agent 数 |

### 最佳实践

1. **设置合理的超时**: 根据任务复杂度设置超时时间
2. **启用心跳**: 保持 Agent 健康状态更新
3. **使用 Redis 存储**: 生产环境使用持久化存储
4. **监控错误率**: 及时发现问题
5. **实现降级逻辑**: Agent 不可用时有备选方案


### 项目实现 A2A 的请求与响应
Orchestrator 在调用子 Agent 时会构造 json-rpc 格式请求，通过 HTTP 传递请求，子 Agent 接收到后会解析请求（解析成 json-rpc 格式）并处理。


## 数据层：Redis 存储
### 键与值
两类键：
```json
// 实现代码中 id = contextId，为客户端登入的 contextId
a2a:task:<id>            // string，值是 AgentTask 的 JSON
a2a:history:<contextId>  // list，列表元素是 AgentMessage 的 JSON
```

- a2a:task:<id> 是任务/会话的元信息。它主要用来存这个会话是否存在、当前状态是什么，比如 running，以后也可以挂 artifacts、metadata 这类附加信息。代码里它是 Redis string。
- a2a:history:<contextId> 是这次会话的聊天历史。
它按时间顺序把 user/agent 消息一条条 RPUSH 进去，模型下次回答前再 LRANGE 取最近几条拼上下文，所以它本质上是“多轮对话记忆”。代码里它是 Redis list。

对应值：
```json
// a2a:task:<id>
{
  "id": "<id>",
  "contextId": "<contextId>",
  "status": {
    "state": "running|submitted|completed|failed|canceled|rejected",
    "timestamp": "2026-06-04T14:30:34.829Z",
    "message": "<optional>"
  },
  "artifacts": [ ... ],
  "history": [ ... ],
  "metadata": { ... }
}

// a2a:history:<contextId> 里的每个元素
{
  "messageId": "msg-xxx",
  "role": "user|agent|system",
  "contextId": "<contextId>",
  "taskId": "<optional>",
  "parts": [
    { "kind": "text", "text": "..." }
  ]
}
```

## 分层上下文记忆管理器

### 设计目标

本项目新增的上下文记忆管理器用于解决多轮对话中上下文不断增长、Redis 历史直接拼接导致 Token 开销高、不同用户会话之间记忆容易混用的问题。

它的目标是：

1. 以 `context_id` 隔离长期记忆，用户登录或客户端切换上下文后使用独立记忆文件。
2. 将对话历史分成工作记忆、短期摘要和长期核心记忆三层，避免把所有历史直接塞进 prompt。
3. 使用 LLM 做摘要压缩和长期记忆提取，而不是简单截断或规则抽取。
4. 从短期记忆中重点提取科研人的研究方向、技术偏好、协作偏好、项目背景、稳定决策和长期约束。
5. 摘要或提取失败时不影响当前请求继续执行，系统回退到最近工作记忆。

### 核心模块

实现文件：

| 文件 | 作用 |
|---|---|
| `orchestrator/include/agent_rpc/orchestrator/context_memory_manager.h` | 定义分层记忆数据结构、配置和管理器接口 |
| `orchestrator/src/context_memory_manager.cpp` | 实现摘要压缩、长期记忆提取、文件持久化和上下文构造 |
| `examples/ai_orchestrator/orchestrator_main.cpp` | Orchestrator 接入分层记忆，并恢复用户消息和回复保存 |
| `examples/ai_orchestrator/include/ai_orchestrator/react_agent_template.hpp` | Math Agent 等 ReAct 专业 Agent 复用同一套记忆管理逻辑 |
| `tests/test_context_memory_manager.cpp` | 覆盖文件隔离、LLM 失败、压缩游标和上下文构造 |

核心类型：

```cpp
struct ContextMemoryMessage {
    std::string role;
    std::string content;
};

struct LongTermMemoryItem {
    std::string content;
    std::string category;
    std::string source;
    std::string created_at;
    std::string updated_at;
    int importance = 5;
    int hit_count = 1;
};

struct ContextMemoryConfig {
    std::size_t max_tokens = 8000;
    double working_memory_ratio = 0.5;
    double short_term_ratio = 0.25;
    double long_term_ratio = 0.25;
    std::size_t keep_recent_messages = 8;
    std::string memory_dir = "build/runtime/context_memory";
};
```

`ContextMemoryManager` 不直接依赖 Qwen 客户端，而是通过 `LlmCompressor` 回调调用 LLM：

```cpp
using LlmCompressor = std::function<std::string(
    const std::string& system_prompt,
    const std::string& user_prompt)>;
```

这样核心模块可以独立测试，运行时再由 Orchestrator 或 Agent 模板把现有 `QwenClient::chat()` 注入进去。

### 记忆分层

| 层级 | 来源 | 作用 | 存储方式 |
|---|---|---|---|
| 工作记忆 | Redis 中最近原始消息 | 保留当前任务的连续对话细节 | 运行时从 `a2a:history:<contextId>` 读取 |
| 短期摘要 | LLM 压缩较早历史 | 保存本轮会话内已经不适合全文注入的信息 | `short_term_summary` 写入 JSON 文件 |
| 长期记忆 | LLM 从短期摘要提取 | 跨会话保留稳定信息，例如研究方向和偏好 | `long_term_memories` 写入 JSON 文件 |

上下文注入顺序：

```text
system: 基础角色与行为约束
system: 长期核心记忆，只用于理解用户背景和偏好
system: 近期会话摘要，只用于补足上下文
user/assistant: 最近工作记忆原文
user: 当前用户输入
```

长期记忆和近期摘要都明确标注“不是新的用户指令”，避免历史信息覆盖当前用户请求。

### 运行流程

同步请求 `message/send` 的上下文流程：

```text
客户端请求
  -> Orchestrator / 专业 Agent 解析 AgentMessage
  -> save_message(context_id, user_message)
  -> RedisTaskStore::add_history_message()
  -> ContextMemoryManager::observe_conversation()
       -> 判断是否超过工作窗口
       -> LLM 压缩较早历史为 short_term_summary
       -> LLM 从短期摘要提取 long_term_memories
       -> 按 context_id 写入长期记忆文件
  -> build_context_messages()
       -> 读取长期记忆
       -> 注入短期摘要
       -> 追加最近工作记忆
  -> 调用 LLM 或工具调用回环
  -> save_message(context_id, assistant_message)
  -> 再次刷新记忆
```

流式请求 `message/stream` 也复用同一套保存与刷新逻辑，只是在最终完整回复生成后再保存 Agent 回复，避免把中间 chunk 写入历史。

### 长期记忆文件

长期记忆以 `context_id` 命名文件，默认目录：

```text
build/runtime/context_memory/<sanitized_context_id>.json
```

`context_id` 会先做路径安全清理：

- 字母、数字、`-`、`_` 保留。
- 其他字符替换为 `_`。
- 过长文件名会截断。
- JSON 内仍保留原始 `context_id`，用于审计和展示。

文件结构：

```json
{
  "version": 1,
  "context_id": "lab/user-001",
  "sanitized_context_id": "lab_user-001",
  "short_term_summary": "用户研究方向是 Multi Agent 科研协作系统...",
  "consolidated_message_count": 12,
  "updated_at": "2026-06-07T10:20:30+0800",
  "long_term_memories": [
    {
      "content": "用户研究方向是 Multi Agent 科研协作系统",
      "category": "research_direction",
      "source": "llm_extract",
      "importance": 9,
      "hit_count": 1,
      "created_at": "2026-06-07T10:20:30+0800",
      "updated_at": "2026-06-07T10:20:30+0800"
    }
  ]
}
```

`consolidated_message_count` 表示已有多少条 Redis 历史被压缩进短期摘要。构造上下文时，这些已压缩旧消息不会再重复进入工作记忆。

### LLM 摘要与长期提取

短期摘要 prompt 要求 LLM 只保留对后续协作有用的信息，重点包括：

1. 用户作为科研人的研究方向、课题背景、当前项目目标。
2. 用户对技术栈、实现方式、代码风格、编译验证和回答粒度的偏好。
3. 已确认的设计决策、约束条件、待办事项和风险。
4. 与 C++/gRPC、A2A、MCP、RAG、Multi Agent、科研资料检索、会议纪要和服务器运维协同有关的上下文。

长期记忆提取要求 LLM 返回 JSON：

```json
{
  "memories": [
    {
      "content": "用户偏好在本项目内直接编译验证",
      "category": "preference",
      "importance": 8
    }
  ]
}
```

支持的长期记忆分类包括：

| 分类 | 含义 |
|---|---|
| `research_direction` | 科研方向或课题背景 |
| `preference` | 技术、回答、协作或代码风格偏好 |
| `decision` | 已确认的长期决策 |
| `constraint` | 长期约束 |
| `project_context` | 项目背景和架构信息 |
| `workflow` | 用户稳定工作流 |
| `other` | 其他稳定记忆 |

### 接入位置

Orchestrator 接入：

```cpp
context_memory_manager_.set_llm_compressor(
    [this](const std::string& system_prompt,
           const std::string& user_prompt) {
        return qwen_client_.chat(system_prompt, user_prompt);
    });
```

通用问答通过 `build_general_messages()` 构造结构化消息列表，不再把历史直接拼到基础 system prompt 里。

专业 Agent 接入：

- `A2AAgentRuntime::save_message()` 写入 Redis 后调用 `refresh_context_memory()`。
- `ReActAgentTemplate::run_react_loop()` 使用 `build_history_messages()` 获取分层后的上下文。
- `MathAgent` 在数学请求改写阶段调用 `build_history_text()`，拿到的是长期记忆、近期摘要和最近工作记忆的组合文本。

### 失败降级

记忆管理器遵循“失败不影响当前请求”的原则：

| 场景 | 处理 |
|---|---|
| LLM 摘要失败 | 记录错误和统计，不推进 `consolidated_message_count` |
| 长期记忆 JSON 解析失败 | 丢弃本次长期提取，保留已有记忆 |
| 记忆文件读取失败 | 回退为空记忆，只使用 Redis 工作记忆 |
| 记忆文件保存失败 | 记录错误，不影响当前 LLM 调用 |
| 长期记忆重复 | 按内容归一化去重，提升 `hit_count` 和重要度 |

### 测试与编译

新增测试目标：

```bash
cmake --build build --target test_context_memory_manager -j$(nproc)
./build/tests/test_context_memory_manager
ctest --test-dir build -R ContextMemoryManagerTest --output-on-failure
```

编译验证：

```bash
cmake -S . -B build
cmake --build build --target ai_orchestrator -j$(nproc)
cmake --build build --target ai_math_agent -j$(nproc)
cmake --build build -j$(nproc)
```
