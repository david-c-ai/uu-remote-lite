@echo off
chcp 65001 >nul
title DirRemote Server (UDP 21116 / 21117)
cd /d "%~dp0"

netsh advfirewall firewall show rule name="DirRemote-UDP" >nul 2>&1
if errorlevel 1 (
    netsh advfirewall firewall add rule name="DirRemote-UDP" dir=in action=allow protocol=UDP localport=21116-21117 >nul 2>&1
)

echo ==================================================================
echo   DirRemote Windows 服务端正在运行 (关闭本窗口即停止)
echo   监听端口: UDP 21116 (信令打洞) / UDP 21117 (零拷贝中继)
echo ==================================================================
dir-server.exe -p 21116 -r 21117
pause
