#!/bin/bash
# ============================================================================
# File: run_inazuma_xdp_slave.sh
# Desc: InazumaKV XDP 旁路加速测试 (Slave 节点) - 终极修复版
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

# 🚀 绝对路径锁定
ROOT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$ROOT_DIR" || { echo "❌ 目录切换失败"; exit 1; }

echo "🧹 1. 清理 Slave 历史环境..."
killall -9 kvstore xdp_gateway 2>/dev/null
# 🚀 精准清理历史 AOF 快照
rm -f "$ROOT_DIR/data/"*.aof "$ROOT_DIR/data/"*.rdb

echo "🚀 2. 启动 InazumaKV Slave (CPU: 0-3)..."
# 重定向 Slave 业务日志
taskset -c 0-3 ./bin/kvstore ./conf/slave.conf > "$ROOT_DIR/slave_kv.log" 2>&1 &
sleep 2

echo -e "\n========================================================================"
echo "✅ Slave 业务进程已启动，并正在等待 Master 握手！(日志输出至 slave_kv.log)"
echo "👉 【操作指引】请现在切换到 Master 机器 (192.168.124.13)，启动主库。"
echo "========================================================================"
read -p "🛑 当 Master 的网关 (xdp_gateway) 成功启动后，请按 [Enter] 键启动 Slave 网关..."

echo "🚀 3. 启动 Slave XDP 旁路网关..."
# 启动 Slave 网关并重定向日志
./bin/xdp_gateway --slave eth_xdp > "$ROOT_DIR/slave_xdp.log" 2>&1 &
sleep 2

echo "✅ Slave XDP 网关已接管流量！请回到 Master 开始压测！(可通过 tail -f slave_xdp.log 观察接收状态)"
wait