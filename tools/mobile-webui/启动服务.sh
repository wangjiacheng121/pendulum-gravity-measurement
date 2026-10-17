#!/bin/sh
# pendulum-g 手机端一键启动（Operit 内置 Ubuntu 终端运行）
cd "$(dirname "$0")" || exit 1
echo "=============================================="
echo " pendulum-g 本地服务已启动"
echo " 请用 Chrome 打开:"
echo "   http://127.0.0.1:8000/ble_cli_web.html"
echo " 停止服务：按 Ctrl+C 或直接关闭终端"
echo "=============================================="
python3 -m http.server 8000
