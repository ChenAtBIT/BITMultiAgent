# 构建与运行说明

项目默认同时构建 DAG Web 服务、MCP Client、MCP Server 和四个核心 C++ Plugin。工具运行时是生产链路的必需部分，不提供旧 Plugin、RAG 工具检索或无工具降级开关。

## 依赖

- CMake 3.20+
- 支持 C++20 的编译器
- libcurl
- jsoncpp
- GoogleTest
- RapidCheck
- iconv（可选；用于非 UTF-8 网页字符集转换）


## 四个核心 Plugin

| Plugin | Actor | 工具 |
| --- | --- | --- |
| `team_design` | Designer | `list_team`、`add_agent`、`update_agent`、`remove_agent`、`commit_team` |
| `dag_control` | Planner | `list_plan`、`add_node`、`update_node`、`remove_node`、`validate_plan`、`commit_plan` |
| `workspace_fs` | Designer、Planner、Executor | 文件和目录的受限 CRUD、移动与文本替换 |
| `web_research` | Designer、Planner、Executor | `web_search`、`fetch_webpage` |

内部 Tool ID 使用 `plugin_id.tool_name`，例如 `workspace_fs.read_file`；注入模型时使用兼容函数名 `workspace_fs__read_file`。Registry 保存并校验二者映射。

默认映射位于 [`config/tool_orchestration.json`](config/tool_orchestration.json)：

```json
{
  "schema_version": 1,
  "modes": {
    "dag_team": {
      "actors": {
        "designer": ["team_design", "workspace_fs", "web_research"],
        "planner": ["dag_control", "workspace_fs", "web_research"],
        "executor": ["workspace_fs", "web_research"]
      }
    }
  },
  "permission": "allow"
}
```

配置中出现未知 Plugin、重复映射、非法 Schema 或 `ask/deny` 时，MCP Server 启动失败，不会降级成无工具模式。

## Session 与工作区

页面首次打开时调用 `POST /api/sessions`，服务端生成 128 位随机 Session ID。前端将 ID 存入 `sessionStorage`；同一页面的多个 Run 复用它，页面刷新时通过 `GET /api/sessions/{id}` 恢复。

默认目录：

```text
sessions/<session_id>/
├── workspace/   # workspace_fs 唯一可访问根目录
├── artifacts/
└── audit/       # tools.jsonl，只记录脱敏元数据
```

所有工具路径必须是相对路径。`workspace_fs` 拒绝绝对路径、`..`、空字节、工作区根删除和软链接穿越；写入使用同目录临时文件和原子重命名。默认限制为单文件 1 MiB、Session 工作区 100 MiB、目录列举 1000 项。

`web_research` 使用 C++ 和 libcurl。只允许 HTTP(S)，保持 TLS 校验并限制重定向、超时、响应大小和输出长度。首版明确允许公网、localhost、私网和链路本地地址。网页结果标记为 `untrusted_web_content`。

Session 目录首版不自动清理。

## 模型工具循环

- Qwen Chat Completions 请求携带当前 ToolView 的完整 `tools`。
- 标准 `tool_calls` 按模型返回顺序执行，响应通过 `tool` message 回传模型。
- 单轮最多进行 8 次工具迭代；工具错误作为结构化结果反馈，模型可以修正参数。
- Designer 必须调用 `team_design__commit_team`，Planner 必须调用 `dag_control__commit_plan`。
- Designer/Planner 首次未提交或提交非法时使用全新 operation 重试一次；第二次失败产生 `run_stalled`。
- 动态业务 Agent 始终映射为可信 `executor`，不使用用户可编辑的 `role` 文本授权。

运行事件包含 `tool_started`、`tool_completed` 和 `tool_failed`。Session 审计记录 Tool ID、可信 Actor、脱敏路径或 URL、耗时和结果大小，不记录文件正文、网页正文、搜索词或密钥。

模型往返日志写入 `log/agent_designer.log`、`log/planner.log` 和 `log/agent_<id>.log`；工具循环的每次迭代都会记录，便于定位未提交、参数修正和迭代上限问题。该诊断日志与 Session 下的脱敏工具审计相互独立。

## 构建与启动

依赖 CMake 3.20+、C++20、libcurl、jsoncpp、GoogleTest 和 RapidCheck。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

启动 Web、MCP Server 和四个 Plugin：

```bash
export QWEN_API_KEY=sk-your-key
./examples/ai_orchestrator/start_system.sh
```

默认地址为 `http://127.0.0.1:8000`。停止服务：

```bash
./examples/ai_orchestrator/stop_system.sh
```

可用环境变量：

| 变量 | 用途 |
| --- | --- |
| `QWEN_API_KEY` | 必需的模型 API Key |
| `QWEN_API_URL` / `QWEN_BASE_URL` | 兼容 Chat Completions 的地址 |
| `QWEN_MODEL` | 模型名，默认 `qwen-plus` |
| `PORT` | Web 端口，默认 `8000` |
| `MCP_SERVER_PATH` | MCP Server 可执行文件 |
| `MCP_PLUGINS_DIR` | 四个 `.so` 所在目录 |
| `TOOL_CONFIG_PATH` | 工具映射 JSON |
| `DAG_SESSIONS_DIR` | Session 根目录 |
| `BING_SEARCH_URL` | Bing 搜索入口，默认 `https://cn.bing.com/search` |

核心 Plugin 或配置加载失败时 `/health` 不会进入可用状态，Web 服务启动也会失败。

## Web API

- `POST /api/sessions`：创建页面 Session。
- `GET /api/sessions/{id}`：验证并恢复 Session。
- `GET /health`：检查 Web 和 MCP 工具运行时。
- `GET /api/agents`：读取默认 Agent Pool。
- `POST /api/agents/draft`：通过 Designer 工具生成草稿。
- `POST /api/materials/parse`：解析 MD/TXT 文本资料。
- `POST /api/runs`：创建 DAG Run。
- `GET /api/runs/{run_id}`：获取属于当前 Session 的 Run Snapshot。
- `POST /api/runs/{run_id}/agents/{agent_id}/retry`：重跑当前 Session 的单个节点。

除 Session 创建/恢复和静态接口外，动态 API 都要求 `X-Session-ID`。跨 Session 查询和 Retry 返回拒绝结果。

## 完整构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

核心目标：

```bash
cmake --build build --target \
  ai_orchestrator mcp_server team_design dag_control workspace_fs web_research \
  -j$(nproc)
```

仅在开发独立模块时可以关闭 MCP Server 构建：

```bash
cmake -S . -B build-no-server -DDAG_MULTI_AGENT_BUILD_MCP_SERVER=OFF
```

这种构建不能启动生产 Web 服务，因为 `ai_orchestrator` 启动时要求 MCP Server 和四个核心 Plugin 可用。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

测试覆盖 DAG/Retry、Session 恢复与隔离、Registry/Schema/角色过滤、文件路径安全、Plugin commit、MCP 并发 request ID，以及基于本地 HTTP Server 的网页抓取、重定向、字符集、Bing fixture、超时和响应上限。测试不访问真实搜索引擎。

## 启动

```bash
export QWEN_API_KEY=sk-your-key
./examples/ai_orchestrator/start_system.sh
```

脚本会构建 Web、MCP Server 和四个 Plugin，然后启动 Web 服务。环境覆盖项包括 `MCP_SERVER_PATH`、`MCP_PLUGINS_DIR`、`TOOL_CONFIG_PATH`、`DAG_SESSIONS_DIR` 和 `BING_SEARCH_URL`。

默认运行路径：

```text
build/mcp_server/mcp_server
build/mcp_server/plugins/{team_design,dag_control,workspace_fs,web_research}.so
config/tool_orchestration.json
sessions/<session_id>/{workspace,artifacts,audit}
```
