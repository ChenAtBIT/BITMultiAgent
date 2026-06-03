#!/bin/bash

# AI Agent 系统停止脚本

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 项目根目录
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# 优先使用统一运行时目录；保留旧目录兜底，避免中断已有进程清理。
PID_DIR="$PROJECT_ROOT/build/runtime/examples/ai_orchestrator/pids"
LEGACY_PID_DIR="$SCRIPT_DIR/pids"

echo "停止 AI Agent 系统..."

# 停止所有服务
for pid_file in "$PID_DIR"/*.pid "$LEGACY_PID_DIR"/*.pid; do
    if [ -f "$pid_file" ]; then
        pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            echo "停止进程 $pid ($(basename $pid_file .pid))"
            kill "$pid"
        fi
        rm -f "$pid_file"
    fi
done

echo "系统已停止"
