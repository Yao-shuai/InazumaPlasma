#!/bin/bash
# ============================================================================
# File: run_official_redis_rdb_bench.sh
# Desc: 针对官方 Redis 的全量持久化 (SAVE vs BGSAVE) 真实高压冲击测试
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

CMD=${1:-none}   # 接收参数：none, save, bgsave
TIMES=${2:-0}    # 接收参数：触发次数 (0, 1, 10)
PORT=6379

# 1. 清理环境，绝对不能让你的 kvstore 和官方 redis 冲突
killall -9 kvstore redis-server redis-benchmark 2>/dev/null
rm -f dump.rdb appendonly.aof official_bench.tmp
echo "🧹 [清理] 已关闭所有引擎，清空官方 Redis 的 dump.rdb。"

# 2. 启动官方 Redis (双核物理隔离机制)
echo "🚀 启动 官方 Redis-Server (无自动持久化模式)..."
taskset -c 0,3 redis-server --port $PORT --save "" --appendonly no --daemonize yes --dir ./ --dbfilename dump.rdb
sleep 2

# 3. 💣 后台地狱引爆器：养肥了再杀
if [ "$CMD" != "none" ] && [ "$TIMES" -gt 0 ]; then
    (
        # 等待 1.5 秒，让内存里先积攒几百兆真实数据
        sleep 1.5 
        for i in $(seq 1 $TIMES); do
            redis-cli -p $PORT $CMD > /dev/null 2>&1
            echo "💥 [System] 官方 Redis 遇袭！精准触发 $CMD ($i/$TIMES)"
            # 每隔 0.5 秒引爆一次，防止并发冲突
            sleep 0.5 
        done
    ) &
fi

# 4. 启动极限压测 (1000万次请求，Pipeline 16，彻底填满内存)
# 🚀 终极修复：使用 SET 命令！保证 100% 成功写入内存，生成几百 MB 真实的 RDB 巨兽！
echo "⚔️ 启动 10,000,000 条海量数据压测对官方 Redis 进行拷问..."
taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 10000000 -r 10000000 -P 16 SET __rand_int__ __rand_int__ 2>/dev/null > official_bench.tmp

# 5. 打印成绩单
echo -e "\n📊 ========== 官方 Redis 成绩单 ($CMD - $TIMES 次) =========="
grep "requests per second" official_bench.tmp
grep -E "100.00%" official_bench.tmp

# 验证官方落盘文件，这次绝对是几百兆的实打实数据！
ls -lh dump.rdb 2>/dev/null | awk '{print "📁 验证官方落盘文件: " $9 " | 大小: " $5}'

# 6. 安全关闭官方 Redis 并销毁文件
killall -9 redis-server 2>/dev/null
rm -f dump.rdb official_bench.tmp
echo "🧹 [清理] 测试结束，官方 Redis 已关闭并销毁 RDB。"
echo -e "====================================================\n"
sleep 3