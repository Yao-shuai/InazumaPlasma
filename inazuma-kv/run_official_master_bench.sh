#!/bin/bash
# ============================================================================
# File: run_official_master_bench.sh
# Desc: 官方 Redis 主从复制 (Replication) 损耗测试 - Master 控制端
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

PORT=6379
PIPELINES=(1 4 8 16 20 40 80 160)

echo "🧹 1. 清理 Master 环境..."
killall -9 kvstore redis-server redis-benchmark 2>/dev/null
rm -f dump.rdb appendonly*

echo "🚀 2. 启动 官方 Redis Master (绑定 CPU 0)..."
taskset -c 0 redis-server --bind 0.0.0.0 --protected-mode no --port $PORT --save "" --appendonly no --daemonize yes --dir ./
sleep 2

echo -e "\n========================================================================"
echo "⏳ [发令枪等待] 官方 Redis Master 已就绪！"
echo -e "👉 【操作指引】请保持当前窗口不动，切换到 \033[31mSlave 机器 (192.168.124.14)\033[0m，"
echo "   并直接复制执行以下 3 行命令来启动从库："
echo "------------------------------------------------------------------------"
echo "sudo killall -9 redis-server kvstore 2>/dev/null"
echo "rm -f dump.rdb appendonly*"
echo "sudo taskset -c 0-3 redis-server --port 6380 --save \"\" --appendonly no --replicaof 192.168.124.13 6379"
echo "------------------------------------------------------------------------"
echo "========================================================================"
read -p "🛑 确认 Slave 已经成功启动并与 Master 建立同步后，请按 [Enter] 键开始疯狂压测..."

echo -e "\n⚔️ 3. 开启高压火力全覆盖测试..."
for P in "${PIPELINES[@]}"; do
    echo -e "\n===================================================="
    echo "🔥 [官方 Redis - 一主一从] 正在测试 Pipeline: $P ..."
    # 压测进程绑在 CPU 1,2，绝对不抢占 Master (CPU 0) 的算力
    taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 1000000 -r 1000000 -P $P SET repl_key:__rand_int__ repl_val:__rand_int__ 2>/dev/null | grep -E "requests per second|100.00%"
    sleep 1
done

echo -e "\n🧹 4. 测试结束，安全关闭 Master..."
killall -9 redis-server
echo "✅ 收工"