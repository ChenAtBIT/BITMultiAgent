# 项目目录重分布规划

## 目标

根据当前 README 中的分层架构，将仓库目录职责收拢为三类：

1. 源码目录：只放长期维护的实现、接口、示例和文档。
2. 构建目录：统一由主项目 `build/` 承载所有二进制、库和插件产物。
3. 运行时目录：日志、PID、临时文件统一落在 `build/runtime/`，避免污染源码树。

## 本次落地后的目录分布

```text
agent-communication/
├── build/
│   ├── mcp_server_integrated/          # MCP Server 与插件构建产物
│   ├── examples/                       # 示例程序二进制
│   ├── server/                         # rpc_server 等主程序产物
│   ├── client/                         # rpc_client 等客户端产物
│   └── runtime/
│       ├── examples/ai_orchestrator/   # 启动脚本生成的日志与 PID
│       └── mcp_server_integrated/      # MCP Server 运行日志
├── common/                             # 公共基础能力
├── proto/                              # 协议定义
├── a2a/                                # A2A 协议实现
├── a2a_adapter/                        # RPC <-> A2A 适配层
├── mcp/                                # MCP 客户端与 RAG-MCP 能力
├── orchestrator/                       # Agent 编排层
├── registry/                           # 服务注册发现
├── server/                             # RPC 服务层
├── client/                             # RPC 客户端入口
├── mcp_server_integrated/              # MCP Server 源码与插件源码
├── examples/                           # 示例源码
├── tests/                              # 测试源码
└── docs/                               # 文档
```

## 对应架构层的目录职责

- 应用入口层：`client/`、`server/`、`examples/`
- 协议与适配层：`proto/`、`a2a/`、`a2a_adapter/`
- 编排与工具层：`orchestrator/`、`mcp/`、`mcp_server_integrated/`
- 基础设施层：`common/`、`registry/`
- 支撑资产：`tests/`、`docs/`

## 目录治理规则

- 不再在源码目录下新增独立 `build/`。
- 示例运行日志不再写入 `examples/.../logs`。
- PID、日志、临时输出统一归档到 `build/runtime/`。
- `mcp_server_integrated` 继续保留为独立源码域，但其构建产物统一由主工程接管到 `build/mcp_server_integrated/`。

## 后续可选演进

如果后面要继续做更强的源码级重构，可以再考虑第二阶段：

- 将 `server/`、`client/`、`examples/` 进一步归并为 `apps/`
- 将 `common/`、`a2a/`、`a2a_adapter/`、`mcp/`、`orchestrator/` 归并为 `modules/`
- 将 `mcp_server_integrated/` 单独提升为 `tools/mcp_server/`

这一步会牵涉较多 include 路径、CMake 和文档引用，建议在功能稳定后再做。
