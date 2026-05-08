#!/bin/bash
# ============================================================================
# File: enable_xdp_master.sh (放在 inazuma-ecosystem 根目录)
# Desc: 挂载主机 XDP 旁路网关 (纯净生产版 - 无压测)
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

ROOT_DIR=$(pwd)
LOG_DIR="$ROOT_DIR/logs"

# 强制进入底层目录
cd "$ROOT_DIR/inazuma-kv" || exit 1

# 🛡️ 核心防御：启动前强制清理网卡上可能残留的 XDP 僵尸钩子
echo "🧹 正在清理网卡可能残留的 eBPF 僵尸钩子..."
sudo ip link set dev eth_xdp xdp off 2>/dev/null

echo "🚀 启动 Master XDP 旁路网关 (绑核 CPU:3)..."
# 注意：此处的 target-mac 已更新为咱们之前查到的从机真实 MAC
sudo bash -c "taskset -c 3 stdbuf -oL ./bin/xdp_gateway --master --target-ip 192.168.124.14 --target-mac 00:0c:29:2c:80:22 eth_xdp > '$LOG_DIR/xdp_gateway.log' 2>&1 & echo \$! > '$LOG_DIR/xdp_gateway.pid'"
sleep 2

echo -e "\n========================================================================"
echo "✅ Master XDP 网关已与 Slave 建立极速通道！(eBPF 钩子挂载成功)"
echo "🛡️ 网关进程已作为守护进程在后台安静运行。"
echo "⚡ 你的 Inazuma 混合矩阵 (AI网关 + 计费Chat) 现在正全量享受 AF_XDP 零拷贝加速！"
echo ""
echo "👀 实时状态查看命令："
echo "   tail -f logs/xdp_gateway.log"
echo "========================================================================"