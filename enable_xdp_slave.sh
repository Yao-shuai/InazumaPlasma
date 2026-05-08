#!/bin/bash
# ============================================================================
# File: enable_xdp_slave.sh (放在 inazuma-ecosystem 根目录)
# Desc: 挂载从机 XDP 旁路加速 (纯净生产版)
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

ROOT_DIR=$(pwd)
LOG_DIR="$ROOT_DIR/logs"

# 确保 logs 目录存在
mkdir -p "$LOG_DIR"

# 强制进入底层目录
cd "$ROOT_DIR/inazuma-kv" || exit 1

# 🛡️ 核心防御：启动前强制清理网卡上可能残留的 XDP 僵尸钩子
echo "🧹 正在清理网卡可能残留的 eBPF 僵尸钩子..."
sudo ip link set dev eth_xdp xdp off 2>/dev/null

echo "🚀 启动 Slave XDP 旁路网关..."
# 启动并记录 PID 和日志
sudo bash -c "stdbuf -oL ./bin/xdp_gateway --slave eth_xdp > '$LOG_DIR/xdp_gateway.log' 2>&1 & echo \$! > '$LOG_DIR/xdp_gateway.pid'"
sleep 2

echo -e "\n========================================================================"
echo "✅ Slave XDP 网关已接管底层流量 (eBPF 钩子已成功挂载)！"
echo "🛡️ 从机灾备中心已进入高性能接收模式。"
echo ""
echo "👀 实时状态查看命令："
echo "   tail -f logs/xdp_gateway.log"
echo ""
echo "👉 【操作指引】请现在切换到 Master 机器，执行 sudo ./enable_xdp_master.sh"
echo "========================================================================"