#!/bin/bash

# AI Agent 系统启动脚本
# 启动顺序: Registry -> Math Agent -> General Agent -> Ops Agent -> Minutes Agent -> Knowledge Agent -> Orchestrator

set -e

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 项目根目录
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# 主构建目录
BUILD_DIR="$PROJECT_ROOT/build"
# 可执行文件目录
BIN_DIR="$BUILD_DIR/examples/ai_orchestrator"
# 运行时产物目录
RUNTIME_DIR="$BUILD_DIR/runtime/examples/ai_orchestrator"
LOG_DIR="$RUNTIME_DIR/logs"
PID_DIR="$RUNTIME_DIR/pids"

# 检查关键可执行文件是否存在
REQUIRED_BINS=(
    "ai_registry_server"
    "ai_math_agent"
    "ai_general_agent"
    "ai_ops_agent"
    "ai_minutes_agent"
    "ai_knowledge_agent"
    "ai_orchestrator"
    "ai_client"
)

for binary in "${REQUIRED_BINS[@]}"; do
    if [ ! -f "$BIN_DIR/$binary" ]; then
        echo "错误: 找不到可执行文件 $BIN_DIR/$binary，请先编译项目"
        echo "  cmake -S $PROJECT_ROOT -B $BUILD_DIR"
        echo "  cmake --build $BUILD_DIR -j\$(nproc)"
        exit 1
    fi
done

# 配置
REGISTRY_PORT="${REGISTRY_PORT:-18506}"
ORCHESTRATOR_PORT="${ORCHESTRATOR_PORT:-15004}"
MATH_AGENT_PORT="${MATH_AGENT_PORT:-15005}"
GENERAL_AGENT_PORT="${GENERAL_AGENT_PORT:-15006}"
OPS_AGENT_PORT="${OPS_AGENT_PORT:-15007}"
MINUTES_AGENT_PORT="${MINUTES_AGENT_PORT:-15008}"
KNOWLEDGE_AGENT_PORT="${KNOWLEDGE_AGENT_PORT:-15009}"
REDIS_HOST="127.0.0.1" # Redis 默认地址
REDIS_PORT=6379 # Redis 默认端口

# MCP Server 配置
MCP_BUILD_DIR="$BUILD_DIR/mcp_server"
MCP_SERVER_PATH="$MCP_BUILD_DIR/mcp_server"
MCP_PLUGINS_PATH="$MCP_BUILD_DIR/plugins"
MCP_LOGS_PATH="$BUILD_DIR/runtime/mcp_server/logs"
ENABLE_MCP="${ENABLE_MCP:-false}"
KNOWLEDGE_BASE_DB_PATH="${KNOWLEDGE_BASE_DB_PATH:-$BUILD_DIR/runtime/examples/ai_orchestrator/knowledge_base/knowledge_base.db}"
export KNOWLEDGE_BASE_DB_PATH

# RAG-MCP 配置 (智能工具选择)
ENABLE_RAG="${ENABLE_RAG:-false}"
RAG_TOP_K="${RAG_TOP_K:-5}"
RAG_THRESHOLD="${RAG_THRESHOLD:-0.3}"
DASHSCOPE_API_KEY="${DASHSCOPE_API_KEY:-}"

# API Key (请替换为你的 API Key)
API_KEY="${QWEN_API_KEY:-sk-your-api-key}"

# 检查 API Key
if [ "$API_KEY" == "sk-your-api-key" ]; then
    echo "警告: 请设置 QWEN_API_KEY 环境变量"
    echo "export QWEN_API_KEY=sk-xxx"
    exit 1
fi

# 创建日志和 PID 目录
mkdir -p "$LOG_DIR" "$PID_DIR"
mkdir -p "$(dirname "$KNOWLEDGE_BASE_DB_PATH")"

cleanup_failed_start() {
    echo ""
    echo "启动失败，正在停止已启动的服务..."
    "$SCRIPT_DIR/stop_system.sh"
}

wait_for_service() {
    local service_name="$1"
    local pid="$2"
    local port="$3"
    local log_file="$4"

    sleep 1

    if ! kill -0 "$pid" 2>/dev/null; then
        echo "错误: $service_name 启动失败 (端口: $port)"
        if [ -f "$log_file" ]; then
            echo "最近日志:"
            tail -n 20 "$log_file"
        fi
        echo "提示: 如果端口被占用，可通过 REGISTRY_PORT / ORCHESTRATOR_PORT / MATH_AGENT_PORT / GENERAL_AGENT_PORT / OPS_AGENT_PORT / MINUTES_AGENT_PORT / KNOWLEDGE_AGENT_PORT 覆盖默认端口"
        cleanup_failed_start
        exit 1
    fi

    echo "$service_name 启动完成 (端口: $port)"
}

echo "=========================================="
echo "AI Agent 系统启动"
echo "=========================================="

# 1. 启动 Registry Server
echo "[1/7] 启动 Registry Server..."
"$BIN_DIR/ai_registry_server" $REGISTRY_PORT > "$LOG_DIR/registry.log" 2>&1 &
registry_pid=$!
echo $registry_pid > "$PID_DIR/registry.pid"
wait_for_service "Registry Server" "$registry_pid" "$REGISTRY_PORT" "$LOG_DIR/registry.log"

# MCP 参数
MCP_ARGS=""
if [ "$ENABLE_MCP" == "true" ] && [ -f "$MCP_SERVER_PATH" ]; then
    # 创建 MCP 日志目录
    mkdir -p "$MCP_LOGS_PATH"
    MCP_ARGS="--enable-mcp --mcp-server $MCP_SERVER_PATH --mcp-args -l,$MCP_LOGS_PATH,-p,$MCP_PLUGINS_PATH"
    echo "MCP 已启用: $MCP_SERVER_PATH"
    echo "MCP 插件目录: $MCP_PLUGINS_PATH"
    echo "MCP 日志目录: $MCP_LOGS_PATH"
    echo "知识库数据库路径: $KNOWLEDGE_BASE_DB_PATH"
fi

# RAG-MCP 参数 (智能工具选择)
RAG_ARGS=""
if [ "$ENABLE_RAG" == "true" ] && [ -n "$DASHSCOPE_API_KEY" ]; then
    RAG_ARGS="--enable-rag --rag-top-k $RAG_TOP_K --rag-threshold $RAG_THRESHOLD"
    echo "RAG-MCP 已启用: 智能工具选择"
    echo "  Top-K: $RAG_TOP_K"
    echo "  相似度阈值: $RAG_THRESHOLD"
elif [ "$ENABLE_RAG" == "true" ] && [ -z "$DASHSCOPE_API_KEY" ]; then
    echo "警告: ENABLE_RAG=true 但未设置 DASHSCOPE_API_KEY，RAG 功能将被禁用"
fi

# 2. 启动 Math Agent
echo "[2/7] 启动 Math Agent..."
"$BIN_DIR/ai_math_agent" math-1 $MATH_AGENT_PORT http://localhost:$REGISTRY_PORT $API_KEY --redis-host $REDIS_HOST --redis-port $REDIS_PORT $MCP_ARGS $RAG_ARGS > "$LOG_DIR/math_agent.log" 2>&1 &
math_agent_pid=$!
echo $math_agent_pid > "$PID_DIR/math_agent.pid"
wait_for_service "Math Agent" "$math_agent_pid" "$MATH_AGENT_PORT" "$LOG_DIR/math_agent.log"

# 3. 启动 General Agent（显式不透传 MCP 参数，保证通用问答不走工具）
echo "[3/7] 启动 General Agent..."
"$BIN_DIR/ai_general_agent" general-1 $GENERAL_AGENT_PORT http://localhost:$REGISTRY_PORT $API_KEY --redis-host $REDIS_HOST --redis-port $REDIS_PORT > "$LOG_DIR/general_agent.log" 2>&1 &
general_agent_pid=$!
echo $general_agent_pid > "$PID_DIR/general_agent.pid"
wait_for_service "General Agent" "$general_agent_pid" "$GENERAL_AGENT_PORT" "$LOG_DIR/general_agent.log"

# 4. 启动 Ops Agent（透传 MCP 参数，负责服务器状态巡检与诊断）
echo "[4/7] 启动 Ops Agent..."
"$BIN_DIR/ai_ops_agent" ops-1 $OPS_AGENT_PORT http://localhost:$REGISTRY_PORT $API_KEY --redis-host $REDIS_HOST --redis-port $REDIS_PORT $MCP_ARGS $RAG_ARGS > "$LOG_DIR/ops_agent.log" 2>&1 &
ops_agent_pid=$!
echo $ops_agent_pid > "$PID_DIR/ops_agent.pid"
wait_for_service "Ops Agent" "$ops_agent_pid" "$OPS_AGENT_PORT" "$LOG_DIR/ops_agent.log"

# 5. 启动 Minutes Agent（透传 MCP 参数，负责会议文件纪要生成）
echo "[5/7] 启动 Minutes Agent..."
"$BIN_DIR/ai_minutes_agent" minutes-1 $MINUTES_AGENT_PORT http://localhost:$REGISTRY_PORT $API_KEY --redis-host $REDIS_HOST --redis-port $REDIS_PORT $MCP_ARGS $RAG_ARGS > "$LOG_DIR/minutes_agent.log" 2>&1 &
minutes_agent_pid=$!
echo $minutes_agent_pid > "$PID_DIR/minutes_agent.pid"
wait_for_service "Minutes Agent" "$minutes_agent_pid" "$MINUTES_AGENT_PORT" "$LOG_DIR/minutes_agent.log"

# 6. 启动 Knowledge Agent（透传 MCP 参数，负责文档向量化入库与知识库问答）
echo "[6/7] 启动 Knowledge Agent..."
"$BIN_DIR/ai_knowledge_agent" knowledge-1 $KNOWLEDGE_AGENT_PORT http://localhost:$REGISTRY_PORT $API_KEY --redis-host $REDIS_HOST --redis-port $REDIS_PORT $MCP_ARGS $RAG_ARGS > "$LOG_DIR/knowledge_agent.log" 2>&1 &
knowledge_agent_pid=$!
echo $knowledge_agent_pid > "$PID_DIR/knowledge_agent.pid"
wait_for_service "Knowledge Agent" "$knowledge_agent_pid" "$KNOWLEDGE_AGENT_PORT" "$LOG_DIR/knowledge_agent.log"

# 7. 启动 Orchestrator（只做路由，不直接使用 MCP）
echo "[7/7] 启动 Orchestrator..."
"$BIN_DIR/ai_orchestrator" orch-1 $ORCHESTRATOR_PORT http://localhost:$REGISTRY_PORT $API_KEY --redis-host $REDIS_HOST --redis-port $REDIS_PORT > "$LOG_DIR/orchestrator.log" 2>&1 &
orchestrator_pid=$!
echo $orchestrator_pid > "$PID_DIR/orchestrator.pid"
wait_for_service "Orchestrator" "$orchestrator_pid" "$ORCHESTRATOR_PORT" "$LOG_DIR/orchestrator.log"

echo ""
echo "=========================================="
echo "系统启动完成!"
echo "=========================================="
echo ""
echo "服务地址:"
echo "  Registry:     http://localhost:$REGISTRY_PORT"
echo "  Orchestrator: http://localhost:$ORCHESTRATOR_PORT"
echo "  Math Agent:   http://localhost:$MATH_AGENT_PORT"
echo "  General Agent:http://localhost:$GENERAL_AGENT_PORT"
echo "  Ops Agent:    http://localhost:$OPS_AGENT_PORT"
echo "  Minutes Agent:http://localhost:$MINUTES_AGENT_PORT"
echo "  Knowledge Agent:http://localhost:$KNOWLEDGE_AGENT_PORT"
echo ""
echo "使用客户端连接:"
echo "  $BIN_DIR/ai_client http://localhost:$ORCHESTRATOR_PORT"
echo ""
echo "查看日志:"
echo "  tail -f $LOG_DIR/orchestrator.log"
echo ""
echo "停止系统:"
echo "  $SCRIPT_DIR/stop_system.sh"
