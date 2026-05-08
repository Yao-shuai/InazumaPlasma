#!/bin/bash
# ============================================================================
# File: run_inazuma_xdp_master.sh
# Desc: InazumaKV XDP 旁路加速测试 (Master 节点 & 压测发令枪) - 终极修复版
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

# 🚀 绝杀 1：绝对路径锁定！无论你在哪里执行该脚本，强制将工作目录切回脚本所在目录！
ROOT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$ROOT_DIR" || { echo "❌ 目录切换失败"; exit 1; }

PORT=6379
PIPELINES=(1 4 8 16 20 40 80 160)

echo "🧹 1. 清理 Master 历史环境..."
killall -9 kvstore xdp_gateway redis-benchmark 2>/dev/null
# 🚀 绝杀 2：精准定位，安全清理，彻底解决 AOF 体积翻倍 Bug！
rm -f "$ROOT_DIR/data/"*.aof "$ROOT_DIR/data/"*.rdb

echo "🚀 2. 启动 InazumaKV Master (CPU: 0)..."
# 🚀 绝杀 3：将业务日志重定向到文件，保持终端纯净！保证 conf 绝对路径加载正确！
taskset -c 0 ./bin/kvstore ./conf/master.conf > "$ROOT_DIR/master_kv.log" 2>&1 &
sleep 3

echo -e "\n========================================================================"
echo "⏳ [第一阶段等待] Master 业务进程已启动！(日志输出至 master_kv.log)"
echo "👉 【操作指引】请确保 Slave 的 kvstore 已启动并打印 'Finished with success' 或 Ping 通。"
echo "========================================================================"
read -p "🛑 确认主从 TCP 握手完成后，请按 [Enter] 键启动 Master XDP 网关..."

echo "🚀 3. 启动 Master XDP 旁路网关..."
# 重定向网关日志，防止高并发下终端被打爆
taskset -c 3 ./bin/xdp_gateway --master --target-ip 192.168.124.14 --target-mac 00:0c:29:de:bc:bf eth_xdp > "$ROOT_DIR/master_xdp.log" 2>&1 &
sleep 2

echo -e "\n========================================================================"
echo "⏳ [第二阶段等待] Master XDP 网关已挂载！(日志输出至 master_xdp.log)"
echo "👉 【操作指引】请切回 Slave 机器，按下回车启动 Slave 网关。"
echo "========================================================================"
read -p "🛑 确认 Slave 网关也已挂载完毕后，请按 [Enter] 键开火压测！..."

echo -e "\n⚔️ 4. 开启 XDP 核级旁路极限压测..."
for P in "${PIPELINES[@]}"; do
    echo -e "\n===================================================="
    echo "🔥 [InazumaKV XDP] 正在测试 Pipeline: $P ..."
    taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 1000000 -r 1000000 -P $P HSET xdp_key:__rand_int__ xdp_val:__rand_int__ 2>/dev/null | grep -E "requests per second|100.00%"
    sleep 1
done

echo -e "\n🧹 5. 测试结束，您可以通过 cat master_xdp.log 查看网关吞吐日志..."
# killall -9 kvstore xdp_gateway
# echo "✅ XDP 数据收集完毕！"