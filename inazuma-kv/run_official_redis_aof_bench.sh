#!/bin/bash
# ============================================================================
# File: run_official_redis_aof_bench.sh
# Desc: 针对官方 Redis AOF 持久化的控制变量法冲击测试 (终极真实数据版)
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

PIPELINE=${1:-1}
PORT=6379
OUT_FILE="official_aof_bench.tmp"

# 1. 物理清场，绝对不留前朝余孽
killall -9 kvstore redis-server redis-benchmark 2>/dev/null
rm -rf appendonly.aof appendonlydir dump.rdb $OUT_FILE
echo "🧹 [清理] 已关闭所有引擎，清空官方 Redis 的历史 AOF。"

# 2. 启动官方 Redis (极其关键的公平配置！)
# --save "": 关闭 RDB 快照
# --appendonly yes: 开启 AOF
# --appendfsync no: 核心配置！让 OS 管理刷盘，完全对齐你的 io_uring 和 write 机制
echo "🚀 启动 官方 Redis-Server (AOF 模式, appendfsync: no)..."
taskset -c 0,3 redis-server --port $PORT --save "" --appendonly yes --appendfsync no --daemonize yes --dir ./ 
sleep 2

# 3. 启动极限压测 (保持与你的 KV 引擎绝对一致：100万请求，固定 Pipeline)
# 🚀 终极修复：使用 SET，并加上 -r 1000000 确保生成真实海量随机数据！
echo "⚔️ 启动 1,000,000 条数据压测对官方 Redis 进行拷问 (Pipeline: $PIPELINE)..."
taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 1000000 -r 1000000 -P $PIPELINE SET __rand_int__ __rand_int__ 2>/dev/null > $OUT_FILE

# 4. 打印成绩单与验证
echo -e "\n📊 ========== 官方 Redis 成绩单 (AOF | Pipeline: $PIPELINE) =========="
grep "requests per second" $OUT_FILE
grep -E "100.00%" $OUT_FILE

# 🚀 核心修复：温柔地让 Redis 自己关闭，它会把内存里的 AOF 乖乖吐到硬盘上
redis-cli -p $PORT shutdown 2>/dev/null
sleep 2

# 检查 Redis 7 的专属文件夹，如果不存在再看单个文件
if [ -d "appendonlydir" ]; then
    du -sh appendonlydir | awk '{print "📁 验证官方落盘文件夹: appendonlydir | 总大小: " $1}'
else
    ls -lh appendonly.aof 2>/dev/null | awk '{print "📁 验证官方落盘文件: " $9 " | 大小: " $5}'
fi

# 5. 安全清理前朝遗老
killall -9 redis-server 2>/dev/null # 保底斩杀
rm -rf appendonlydir appendonly.aof $OUT_FILE
echo "🧹 [清理] 测试结束，官方 Redis 已关闭并销毁 AOF。"
echo -e "====================================================\n"
sleep 2