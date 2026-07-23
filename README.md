# C++ DAG Web 编排器

`ai_orchestrator` 现在是一个单进程 C++ Web 服务。它复用 Agent-DAG-Orchestrator 的动态 Agent、Planner、DAG 调度、ReAct Prompt、skill 注入和 Retry 交互；旧的意图识别与多进程路由不再是网页主流程。

## 启动

```bash
export QWEN_API_KEY=sk-your-key
export QWEN_API_URL=https://your-compatible-endpoint/compatible-mode/v1
./examples/ai_orchestrator/start_system.sh
```

默认访问地址为 `http://127.0.0.1:8000`。`QWEN_API_URL` 可以是兼容接口的 base URL，也可以直接是 `/chat/completions` URL；不设置时沿用 B 原有 DashScope 地址。可通过 `PORT` 修改端口。

停止服务：

```bash
./examples/ai_orchestrator/stop_system.sh
```

启动脚本会自动配置/构建 `build`，并只启动一个 Web 进程。运行日志统一位于项目根目录的 `log/`：`service.log` 保存 Web/DAG 命令行日志，`agent_<agent_id>.log` 保存对应 Agent 的 ReAct 对话，`planner.log` 和 `agent_designer.log` 保存 Planner/Agent Designer 的模型请求。每次启动脚本时会清空并重新创建 `log/` 内容；服务已在运行时不会清空当前日志。

Agent 日志按 JSON 记录每次模型请求，包含 `phase`、`run_id`、Agent、轮次、temperature、提交给模型的完整 `request.messages` 和原始 `response`。API Key 不写入这些日志。
如果输入资料或兼容接口返回中出现非法 UTF-8 字节，日志会将对应字节保留为 `\\xHH` 转义；日志异常不会再改变 Agent 节点的执行结果。

当前版本暂时关闭 `limit_text` 的长度截断，ReAct 记忆、依赖输出和 Agent 输出会完整传递；仅对非法 UTF-8 字节做传输安全转义。
当前 ReAct 上下文压缩默认关闭，轮次之间保留完整累积记忆；最大 ReAct 轮次限制仍然有效。

## Web API

- `GET /health`：健康检查。
- `GET /api/agents`：读取默认 Agent 池。
- `POST /api/agents/draft`：根据任务生成可编辑 Agent 草稿。
- `POST /api/runs`：创建 DAG 运行。
- `GET /api/runs/{run_id}`：轮询运行状态、Plan、事件和输出。
- `POST /api/runs/{run_id}/agents/{agent_id}/retry`：携带编辑后的输出和反馈重跑单节点。
- `POST /api/materials/parse`：解析 MD/TXT 的 base64 文本资料。

前端不接收 API Key。模型配置从进程环境读取，运行快照会隐藏资料原文和密钥。

## 测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

`tests/test_dag_orchestrator.cpp` 使用注入的确定性模型，不访问网络，也不需要 API Key。
