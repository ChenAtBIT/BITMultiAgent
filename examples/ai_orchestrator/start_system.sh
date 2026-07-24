#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
BIN="$BUILD_DIR/examples/ai_orchestrator/ai_orchestrator"
RUNTIME_DIR="$BUILD_DIR/runtime/examples/ai_orchestrator"
LOG_DIR="$PROJECT_ROOT/log"
SERVICE_LOG="$LOG_DIR/service.log"
PID_FILE="$RUNTIME_DIR/ai_orchestrator.pid"
PORT="${PORT:-8000}"

if [[ -z "${QWEN_API_KEY:-}" ]]; then
  echo "错误: 启动前必须设置 QWEN_API_KEY。" >&2
  echo "示例: export QWEN_API_KEY=sk-..." >&2
  exit 2
fi

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "[1/2] 配置项目..."
  cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
fi

echo "[1/2] 构建 Web、MCP Server 和四个核心 Plugin..."
cmake --build "$BUILD_DIR" --target \
  ai_orchestrator mcp_server team_design dag_control workspace_fs web_research \
  -j"${JOBS:-$(nproc)}"

if [[ -f "$PID_FILE" ]] && kill -0 "$(<"$PID_FILE")" 2>/dev/null; then
  echo "ai_orchestrator 已在运行，PID=$(<"$PID_FILE")"
  echo "访问地址: http://127.0.0.1:${PORT}"
  exit 0
fi

if [[ -e "$LOG_DIR" && ! -d "$LOG_DIR" ]]; then
  echo "错误: 日志路径不是目录: $LOG_DIR" >&2
  exit 1
fi
mkdir -p "$LOG_DIR" "$RUNTIME_DIR"
# A new start owns a fresh log set. Keep the directory itself so its path is
# stable, but remove all files and subdirectories from the previous run.
find "$LOG_DIR" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +

echo "[2/2] 启动 C++ DAG Web 服务..."
DAG_LOG_DIR="$LOG_DIR" "$BIN" "$PORT" >"$SERVICE_LOG" 2>&1 &
PID=$!
echo "$PID" >"$PID_FILE"

cleanup() {
  if kill -0 "$PID" 2>/dev/null; then kill "$PID" 2>/dev/null || true; fi
  rm -f "$PID_FILE"
}
trap cleanup EXIT INT TERM

for _ in {1..30}; do
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "启动失败，最近日志:" >&2
    tail -n 30 "$SERVICE_LOG" >&2 || true
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then break; fi
  sleep 0.2
done

if ! curl -fsS "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
  echo "启动失败: Web 服务未通过 health 检查" >&2
  tail -n 30 "$SERVICE_LOG" >&2 || true
  exit 1
fi

trap - EXIT
echo "系统启动完成。"
echo "访问地址: http://127.0.0.1:${PORT}"
echo "日志目录: $LOG_DIR"
echo "服务日志: $SERVICE_LOG"
echo "停止: $SCRIPT_DIR/stop_system.sh"
