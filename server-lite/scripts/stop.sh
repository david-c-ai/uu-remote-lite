#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
PID_FILE="./server.pid"

if [ -f "$PID_FILE" ]; then
  PID=$(cat "$PID_FILE")
  if kill -0 "$PID" 2>/dev/null; then
    kill "$PID"
    echo "[dir-server] 已停止进程 (PID: $PID)"
  else
    echo "[dir-server] 进程未在运行"
  fi
  rm -f "$PID_FILE"
else
  pkill -f "./dir-server" 2>/dev/null && echo "[dir-server] 已停止运行中的服务" || echo "[dir-server] 当前无运行中的实例"
fi
