#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
PID_FILE="./server.pid"
LOG_FILE="./server.log"

if systemctl is-active --quiet dir-server 2>/dev/null; then
  echo "=== systemd 守护服务运行中 ==="
  systemctl status dir-server --no-pager -n 10
  exit 0
fi

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "[RUNNING] dir-server 后台运行中 (PID: $(cat "$PID_FILE"))"
else
  echo "[STOPPED] dir-server 当前未运行 (可执行 ./start.sh 启动)"
fi

if [ -f "$LOG_FILE" ]; then
  echo "--- 最近 10 条运行与流量统计日志 ---"
  tail -n 10 "$LOG_FILE"
fi
