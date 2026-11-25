#!/usr/bin/env bash
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "请使用 root 权限执行: sudo ./install-systemd.sh"
  exit 1
fi

cd "$(dirname "$0")"
INSTALL_DIR="/opt/dir-server"
mkdir -p "$INSTALL_DIR"
cp -f ./dir-server "$INSTALL_DIR/dir-server"
chmod +x "$INSTALL_DIR/dir-server"

if [ -f "./server.pid" ]; then
  kill "$(cat ./server.pid)" 2>/dev/null || true
  rm -f "./server.pid"
fi

cat > /etc/systemd/system/dir-server.service << 'EOF'
[Unit]
Description=DirRemote Rendezvous (hbbs) & Zero-Copy Relay (hbbr) Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/dir-server
ExecStart=/opt/dir-server/dir-server -p 21116 -r 21117
Restart=always
RestartSec=2
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
EOF

if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
  ufw allow 21116:21117/udp >/dev/null 2>&1 || true
fi
if command -v firewall-cmd >/dev/null 2>&1 && systemctl is-active --quiet firewalld; then
  firewall-cmd --permanent --add-port=21116-21117/udp >/dev/null 2>&1 || true
  firewall-cmd --reload >/dev/null 2>&1 || true
fi

systemctl daemon-reload
systemctl enable --now dir-server
echo "=================================================================="
echo "  DirRemote systemd 开机自启守护服务已安装并启动!"
echo "  服务状态查看: systemctl status dir-server"
echo "  实时日志查看: journalctl -u dir-server -f"
echo "=================================================================="
