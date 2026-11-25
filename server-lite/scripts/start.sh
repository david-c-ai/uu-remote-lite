#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

HBBS_PORT="${1:-21116}"
HBBR_PORT="${2:-21117}"
PID_FILE="./server.pid"
LOG_FILE="./server.log"

chmod +x ./dir-server

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "[dir-server] 服务已在运行中 (PID: $(cat "$PID_FILE"))"
  echo "查看状态请运行: ./status.sh  |  停止请运行: ./stop.sh"
  exit 0
fi

nohup ./dir-server -p "$HBBS_PORT" -r "$HBBR_PORT" >> "$LOG_FILE" 2>&1 &
NEW_PID=$!
echo "$NEW_PID" > "$PID_FILE"
sleep 1

if ! kill -0 "$NEW_PID" 2>/dev/null; then
  echo "[ERROR] 启动失败，请检查端口 UDP $HBBS_PORT / $HBBR_PORT 是否被占用。日志如下："
  tail -n 20 "$LOG_FILE" || true
  exit 1
fi

PUB_IP=$(curl -s --max-time 3 https://api.ipify.org || curl -s --max-time 3 https://ifconfig.me || echo "<你的服务器公网IP>")
LAN_IP=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "127.0.0.1")

echo "=================================================================="
echo "  DirRemote 服务端已一键后台启动成功! (PID: $NEW_PID)"
echo "------------------------------------------------------------------"
echo "  公网连接地址 (填到客户端):  $PUB_IP"
echo "  内网连接地址 (局域网测试):  $LAN_IP"
echo "  信令打洞端口: UDP $HBBS_PORT   |   中继转发端口: UDP $HBBR_PORT"
echo "------------------------------------------------------------------"
echo "  [重要提醒] 请在云服务器控制台【安全组/防火墙】放行:"
echo "             协议: UDP   端口范围: ${HBBS_PORT}-${HBBR_PORT}"
echo "------------------------------------------------------------------"
echo "  常用命令:"
echo "    查看实时状态与流量:  ./status.sh"
echo "    停止后台服务:        ./stop.sh"
echo "    配置开机自启守护:    sudo ./install-systemd.sh"
echo "=================================================================="
