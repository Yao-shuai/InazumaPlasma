#!/bin/bash
# ============================================================================
# File: run_official_redis_none_bench.sh
# Desc: 获取官方 Redis 纯内存基准 (None) 的各级 Pipeline 数据 (终极真实数据版)
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

PIPELINE=${1:-1}
PORT=6379
OUT_FILE="official_none_bench.tmp"

# 1. 物理清场
killall -9 kvstore redis-server redis-benchmark 2>/dev/null
rm -rf appendonly.aof appendonlydir dump.rdb $OUT_FILE

# 2. 启动官方 Redis (双双关闭！无 RDB, 无 AOF)
echo "🚀 启动 官方 Redis-Server (纯内存 None 模式)..."
taskset -c 0,3 redis-server --port $PORT --save "" --appendonly no --daemonize yes --dir ./ 
sleep 2

# 3. 启动极限压测
# 🚀 终极修复：加入 -r 1000000 并改为 SET 命令，确保产生一百万个互不相同的新 Key 吃满内存！
echo "⚔️ 启动 1,000,000 条数据压测官方 Redis 纯内存 (Pipeline: $PIPELINE)..."
taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 1000000 -r 1000000 -P $PIPELINE SET __rand_int__ __rand_int__ 2>/dev/null > $OUT_FILE

# 4. 打印成绩单
echo -e "\n📊 ========== 官方 Redis 成绩单 (None | Pipeline: $PIPELINE) =========="
grep "requests per second" $OUT_FILE

# 5. 安全关闭
killall -9 redis-server 2>/dev/null
rm -f $OUT_FILE