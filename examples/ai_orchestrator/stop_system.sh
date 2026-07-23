#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PID_FILE="${BUILD_DIR:-$PROJECT_ROOT/build}/runtime/examples/ai_orchestrator/ai_orchestrator.pid"

if [[ ! -f "$PID_FILE" ]]; then
  echo "ai_orchestrator 未运行。"
  exit 0
fi

PID="$(<"$PID_FILE")"
if kill -0 "$PID" 2>/dev/null; then
  kill "$PID"
  for _ in {1..20}; do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.1
  done
fi
rm -f "$PID_FILE"
echo "ai_orchestrator 已停止。"
