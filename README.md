## 项目概述
项目背景：针对科研学者在资料检索场景中存在的信息分散、人工处理效率低等问题，实现基于gRPC+A2A+MCP的Multi Agent协作系统。

主要工作：
- 系统通信框架：构建Client -> RPC Server -> A2A Adapter -> Orchestrator -> Worker Agent -> MCP Tool的完整调用链路。

- 动态 DAG 编排：遵循A2A协议实现Agent间通信；针对输入任务，系统先按任务需求动态生成本次所需Agent及其依赖关系，再通过在线拓扑调度按DAG并发执行各节点，突破单Agent在上下文容量与能力边界上的限制。

- 任务规划：设计“思考 -> 工具调用 -> 观察 -> 再思考”的ReAct闭环执行范式，增强Agent对复杂任务的分解、推理和工具调用能力。

- 分层记忆管理器：将对话信息划分为即时工作上下文、会话级短期摘要和持久化长期核心记忆三层，分层控制Token开销，并支持跨轮次、跨会话的知识延续；在10组长上下文测试中，将平均Prompt长度从75120压至31456，平均压缩率达58.1%。

- Agent能力扩展机制：基于MCP协议构建工具调用框架（MCP Client/Server），集成文件读写、服务器巡检、文档向量化与检索等20+个工具，可通过运行时动态库加载实现可插拔扩展。

- 智能工具选择：基于领域工具白名单，设计并实现检索增强生成（RAG）Top-k工具按需选择机制，使100条工具类请求的平均工具注入数由6个降至2.5个，整体工具调用成功率提升8%。

### 项目结构

```text
agent-communication/
├── build/
│   ├── mcp_client/                     # MCP 客户端库与 RAG-MCP 构建产物
│   ├── examples/                       # 示例程序二进制
│   ├── mcp_server/                     # MCP Server 与插件构建产物
│   ├── RPC_server/                     # rpc_server 等主程序产物
│   ├── RPC_client/                     # rpc_client 等客户端产物
│   └── runtime/
│       ├── examples/ai_orchestrator/   # 启动脚本生成的日志与 PID
│       └── mcp_server/                 # MCP Server 运行日志
├── common/                             # 公共基础能力
├── proto/                              # 协议定义
├── a2a/                                # A2A 协议实现
├── a2a_adapter/                        # RPC <-> A2A 适配层
├── mcp_client/                         # MCP 客户端与 RAG-MCP 能力
├── orchestrator/                       # Agent 编排层
├── registry/                           # 服务注册发现
├── RPC_server/                         # RPC 服务层
├── RPC_client/                         # RPC 客户端入口
├── mcp_server/                         # MCP Server 源码与插件源码
├── examples/                           # 示例源码
├── tests/                              # 测试源码
└── docs/                               # 文档
```