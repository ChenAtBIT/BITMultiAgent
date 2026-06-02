# 服务端与客户端创建后消息执行流程

本文基于项目当前默认交互方式分析：

- 客户端使用 `build/client/rpc_client`
- 服务端使用 `build/server/rpc_server`
- 按默认同步模式分析，也就是没有执行 `/stream` 命令时的调用路径

---

## 1. 服务端和客户端创建阶段

### 1.1 服务端创建流程

服务端入口在 `server/src/main.cpp`。

核心创建链路如下：

```text
main
-> RpcServer server
-> server.setA2AConfig(a2a_config)
-> server.initialize(config)
   -> RpcServer::initialize
   -> 创建 AIQueryServiceImpl
   -> AIQueryServiceImpl::initialize
   -> A2AAdapter::initialize
   -> 创建 a2a::A2AClient(orchestrator_url)
   -> RpcServer::setupServer
   -> builder.RegisterService(ai_query_service_impl_.get())
-> server.start()
```

对应代码位置：

- `server/src/main.cpp:137-156`
- `server/src/rpc_server.cpp:74-95`
- `server/src/ai_query_service.cpp:29-51`
- `a2a_adapter/src/a2a_adapter.cpp:29-49`
- `server/src/rpc_server.cpp:222-267`

### 1.2 客户端创建流程

客户端入口在 `client/src/main.cpp`。

核心创建链路如下：

```text
main
-> RpcClient client
-> client.initialize(config)
-> client.connect(...) / client.connectViaRegistry(...)
   -> RpcClient::connectToEndpoint
   -> RpcClient::setupChannel
   -> AIQueryClient::connect
   -> 创建 AIQueryService gRPC Stub
```

对应代码位置：

- `client/src/main.cpp:162-193`
- `client/src/rpc_client.cpp:712-750`
- `client/src/rpc_client.cpp:590-620`
- `client/src/ai_query_client.cpp:25-60`

创建完成后，用户输入会从 `client/src/main.cpp` 的主循环进入：

- `client/src/main.cpp:202-291`

---

## 2. 用户发送“你好”后的执行流程

这里假设用户在客户端直接输入：

```text
你好
```

### 2.1 总体执行链路

```text
用户输入“你好”
-> client main 循环读取输入
-> RpcClient::aiQuery
-> AIQueryClient::query
-> gRPC: AIQueryServiceImpl::Query
-> A2AAdapter::processQuery
-> RequestAdapter::convertToA2A
-> a2a::A2AClient::send_message
-> HTTP JSON-RPC 到 Orchestrator
-> AIOrchestrator::handle_request
-> AIOrchestrator::analyze_intent
-> AIOrchestrator::handle_general_query
-> QwenClient::chat
-> 响应原路返回客户端
```

### 2.2 分步骤说明

1. 客户端读取用户输入。

   `client/src/main.cpp:254-289` 中，输入 `"你好"` 后会走同步分支：

   ```text
   client.aiQuery(line, context_id, timeout_seconds)
   ```

2. `RpcClient::aiQuery` 转发到 `AIQueryClient`。

   `client/src/rpc_client.cpp:842-874`

   核心调用：

   ```text
   RpcClient::aiQuery
   -> ai_query_client_->query(question, context_id, timeout_seconds)
   ```

3. `AIQueryClient::query` 构造 `AIQueryRequest` 并发起 gRPC 调用。

   `client/src/ai_query_client.cpp:80-153`

   核心调用：

   ```text
   AIQueryClient::query(question, context_id, timeout_seconds)
   -> request.set_question("你好")
   -> stub_->Query(&context, request, &response)
   ```

4. 服务端 `AIQueryServiceImpl::Query` 接收请求。

   `server/src/ai_query_service.cpp:71-138`

   核心调用：

   ```text
   AIQueryServiceImpl::Query
   -> a2a_adapter_->processQuery(*request, response)
   ```

5. `A2AAdapter` 把 RPC 请求转换成 A2A 请求。

   `a2a_adapter/src/a2a_adapter.cpp:61-126`
   `a2a_adapter/src/request_adapter.cpp:18-42`

   核心调用：

   ```text
   A2AAdapter::processQuery
   -> RequestAdapter::convertToA2A
   -> RequestAdapter::buildAgentMessage("你好", context_id, MessageRole::User)
   ```

6. A2A 客户端把请求发给 Orchestrator。

   `a2a/src/client/a2a_client.cpp:79-103`

   核心调用：

   ```text
   a2a::A2AClient::send_message
   -> Impl::send_rpc_request(A2AMethods::MESSAGE_SEND, params_json)
   -> http_client_.post(base_url_, request_json, "application/json")
   ```

7. Orchestrator 收到 `message/send` 请求并解析用户文本。

   `examples/ai_orchestrator/orchestrator_main.cpp:160-220`

   核心调用：

   ```text
   AIOrchestrator::handle_request
   -> JsonRpcRequest::from_json(body)
   -> AgentMessage::from_json(...)
   -> save_message(context_id, message)
   ```

8. Orchestrator 识别意图，`"你好"` 会进入通用对话分支。

   `examples/ai_orchestrator/orchestrator_main.cpp:384-400`

   核心调用：

   ```text
   AIOrchestrator::analyze_intent
   -> qwen_client_.chat("", prompt)
   -> 返回 general
   ```

9. 通用对话由 `handle_general_query` 处理。

   `examples/ai_orchestrator/orchestrator_main.cpp:452-480`

   核心调用：

   ```text
   AIOrchestrator::handle_general_query
   -> task_store_->get_history(context_id, 5)
   -> tryMCPTools(query)            // 可能不命中任何工具
   -> qwen_client_.chat(history_text, "你好")
   ```

10. Orchestrator 生成 A2A 响应并返回。

    `examples/ai_orchestrator/orchestrator_main.cpp:202-211`

    核心调用：

    ```text
    AgentMessage::create()
    -> response_msg.add_text_part(response_text)
    -> JsonRpcResponse::create_success(request.id(), response_msg.to_json())
    ```

11. 服务端把 A2A 响应转换回 RPC 响应。

    `a2a_adapter/src/response_adapter.cpp:17-76`

    核心调用：

    ```text
    ResponseAdapter::convertFromA2A
    -> rpc_response->set_answer(extractTextContent(message))
    ```

12. 客户端拿到结果并打印。

    `client/src/main.cpp:279-288`

    核心调用：

    ```text
    auto response = client.aiQuery(...)
    -> std::cout << response.answer()
    ```

### 2.3 “你好”场景的关键函数调用链

```text
main
-> RpcClient::aiQuery
-> AIQueryClient::query
-> AIQueryServiceImpl::Query
-> A2AAdapter::processQuery
-> RequestAdapter::convertToA2A
-> a2a::A2AClient::send_message
-> AIOrchestrator::handle_request
-> AIOrchestrator::analyze_intent
-> AIOrchestrator::handle_general_query
-> QwenClient::chat
-> ResponseAdapter::convertFromA2A
```

---

## 3. 用户发送数学表达式后的执行流程

这里假设用户输入的是：

```text
1+7
```

或者：

```text
12*(3+4)
```

前半段链路与“你好”完全相同，差异从 Orchestrator 的意图识别开始。

### 3.1 总体执行链路

```text
用户输入数学表达式
-> client main 循环读取输入
-> RpcClient::aiQuery
-> AIQueryClient::query
-> gRPC: AIQueryServiceImpl::Query
-> A2AAdapter::processQuery
-> a2a::A2AClient::send_message
-> AIOrchestrator::handle_request
-> AIOrchestrator::analyze_intent
-> AIOrchestrator::call_math_agent
-> RegistryClient::select_agent_by_tag("math")
-> HTTP JSON-RPC 到 MathAgent
-> MathAgent::handle_request
-> MathAgent::solve_math
-> MathAgent::tryMCPCalculation
-> mcp_integration_->callTool("calculator", ...)
-> QwenClient::chat
-> 响应原路返回客户端
```

### 3.2 分步骤说明

1. 客户端输入和 gRPC 请求发起阶段不变。

   复用的核心函数仍然是：

   ```text
   client/src/main.cpp -> RpcClient::aiQuery
   -> AIQueryClient::query
   -> stub_->Query
   -> AIQueryServiceImpl::Query
   -> A2AAdapter::processQuery
   -> a2a::A2AClient::send_message
   ```

2. Orchestrator 收到请求后先做意图识别。

   `examples/ai_orchestrator/orchestrator_main.cpp:185-200`
   `examples/ai_orchestrator/orchestrator_main.cpp:384-400`

   数学表达式通常会被识别为 `math`：

   ```text
   AIOrchestrator::analyze_intent
   -> qwen_client_.chat("", prompt)
   -> 返回 math
   ```

3. Orchestrator 进入数学 Agent 分支。

   `examples/ai_orchestrator/orchestrator_main.cpp:191-193`
   `examples/ai_orchestrator/orchestrator_main.cpp:402-449`

   核心调用：

   ```text
   AIOrchestrator::call_math_agent
   -> AIOrchestrator::call_agent_by_tag("math", query, context_id)
   ```

4. Orchestrator 通过注册中心发现 Math Agent。

   `a2a/include/a2a/examples/registry_client.hpp:90-106`
   `a2a/include/a2a/examples/registry_client.hpp:59-73`

   核心调用：

   ```text
   RegistryClient::select_agent_by_tag("math")
   -> find_agents_by_tag("math")
   -> post("/v1/agent/find", ...)
   ```

5. Orchestrator 通过 HTTP/A2A 把同一条用户消息转发给 Math Agent。

   `examples/ai_orchestrator/orchestrator_main.cpp:419-441`

   核心调用：

   ```text
   SimpleHttpClient::post(agent_url, request.dump())
   ```

6. Math Agent 接收 `message/send` 请求。

   `examples/ai_orchestrator/math_agent_main.cpp:119-163`

   核心调用：

   ```text
   MathAgent::handle_request
   -> JsonRpcRequest::from_json(body)
   -> AgentMessage::from_json(...)
   -> save_message(context_id, message)
   -> solve_math(user_text, context_id)
   ```

7. `solve_math` 会优先尝试 MCP 计算能力。

   `examples/ai_orchestrator/math_agent_main.cpp:272-303`

   核心调用：

   ```text
   MathAgent::solve_math
   -> task_store_->get_history(context_id, 5)
   -> tryMCPCalculation(question)
   ```

8. `tryMCPCalculation` 会优先找 `calculator` 工具。

   `examples/ai_orchestrator/math_agent_main.cpp:310-368`

   关键逻辑：

   ```text
   if (mcp_integration_->isRAGEnabled()) {
       mcp_integration_->getRelevantTools(question, 5)
   } else {
       mcp_integration_->hasToolAvailable("calculator")
   }

   -> mcp_integration_->callTool(tool.name, args.dump())
   ```

   其中发送给工具的核心参数是：

   ```json
   {
     "expression": "1+7"
   }
   ```

9. Math Agent 将工具结果作为参考，再调用大模型组织最终回答。

   `examples/ai_orchestrator/math_agent_main.cpp:294-302`
   `a2a/include/a2a/examples/qwen_client.hpp:35-84`

   核心调用：

   ```text
   QwenClient::chat(system_prompt + history_text + tool_result, question)
   ```

   也就是说，当前实现不是“工具结果直接原样返回”，而是：

   ```text
   calculator 结果
   -> 拼进 system prompt
   -> 再由 Qwen 生成最终自然语言答案
   ```

10. Math Agent 返回 A2A 响应给 Orchestrator。

    `examples/ai_orchestrator/math_agent_main.cpp:147-154`

    核心调用：

    ```text
    AgentMessage::create()
    -> response_msg.add_text_part(response_text)
    -> JsonRpcResponse::create_success(request.id(), response_msg.to_json())
    ```

11. Orchestrator 从 Math Agent 响应中提取文本，并作为自己的响应返回。

    `examples/ai_orchestrator/orchestrator_main.cpp:438-444`

    核心调用：

    ```text
    response_json["result"]["parts"][0]["text"]
    -> return 给上游 A2AClient
    ```

12. 后续返回链路与“你好”场景一致。

    ```text
    ResponseAdapter::convertFromA2A
    -> AIQueryServiceImpl::Query 返回 gRPC 响应
    -> AIQueryClient::query 收到 response
    -> client main 打印 response.answer()
    ```

### 3.3 数学表达式场景的关键函数调用链

```text
main
-> RpcClient::aiQuery
-> AIQueryClient::query
-> AIQueryServiceImpl::Query
-> A2AAdapter::processQuery
-> a2a::A2AClient::send_message
-> AIOrchestrator::handle_request
-> AIOrchestrator::analyze_intent
-> AIOrchestrator::call_math_agent
-> RegistryClient::select_agent_by_tag
-> MathAgent::handle_request
-> MathAgent::solve_math
-> MathAgent::tryMCPCalculation
-> MCPAgentIntegration::callTool("calculator", ...)
-> QwenClient::chat
-> ResponseAdapter::convertFromA2A
```

---

## 4. 两种输入的核心差异

### 4.1 发送“你好”

特点：

- 请求会停留在 Orchestrator 内部处理
- 走 `general` 分支
- 由 `AIOrchestrator::handle_general_query` 直接调用 `QwenClient::chat`
- 一般不会转发到 `MathAgent`

### 4.2 发送数学表达式

特点：

- 请求先由 Orchestrator 做意图识别
- 命中 `math` 分支后转发给 `MathAgent`
- `MathAgent` 会优先尝试 MCP `calculator` 工具
- 工具结果不会直接作为最终响应返回，而是会作为提示上下文交给 `QwenClient::chat`

---

## 5. 补充说明

1. 本文分析的是默认同步链路。

   如果客户端先输入 `/stream`，入口会变成：

   ```text
   RpcClient::aiQueryStream
   -> AIQueryClient::queryStream
   -> AIQueryServiceImpl::QueryStream
   -> A2AAdapter::processQueryStreaming
   -> Orchestrator::handle_stream_request
   ```

2. 数学表达式是否一定命中 `MathAgent`，取决于 `AIOrchestrator::analyze_intent` 的模型判断结果。

   当前实现中，这一步是通过：

   ```text
   qwen_client_.chat("", prompt)
   ```

   来判断 `math / code / general`。

3. 数学表达式是否一定调用 MCP 工具，取决于：

- Math Agent 是否启用了 MCP
- `calculator` 工具是否可用
- RAG 检索结果是否命中相关工具

如果这些条件不满足，`MathAgent::solve_math` 仍然会继续调用 `QwenClient::chat` 给出答案。
