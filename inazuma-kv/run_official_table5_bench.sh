#!/bin/bash
# ============================================================================
# File: run_official_table5_bench.sh
# Desc: 官方 Redis 全量持久化 (SAVE / BGSAVE) 外部高频注入打榜脚本 (公平 SET 版)
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

PORT=6379
PIPELINE=16
OUT_DIR="test_results/official_table5"
mkdir -p $OUT_DIR

# 测试矩阵: (触发次数_模式)
CONFIGS=(
    "0_NONE"
    "1_SAVE"
    "10_SAVE"
    "100_SAVE"
    "1000_SAVE"
    "1_BGSAVE"
    "10_BGSAVE"
    "100_BGSAVE"
    "1000_BGSAVE"
)

echo -e "================================================================"
echo -e " 🏆 Official Redis 全量持久化 (SAVE / BGSAVE) 自动化测试工具"
echo -e "================================================================\n"

for conf in "${CONFIGS[@]}"; do
    IFS='_' read -r TRIGGERS MODE <<< "$conf"
    
    echo -e "⚙️ 正在拷问官方 Redis: \033[36m模式: $MODE | 外部强行触发 $TRIGGERS 次\033[0m"

    # 1. 物理清场
    killall -9 redis-server redis-benchmark 2>/dev/null
    rm -f dump.rdb appendonly.aof appendonlydir/* 2>/dev/null
    sleep 1

    # 2. 启动纯净版官方 Redis (完全关闭自带的自动触发机制)
    taskset -c 0 redis-server --port $PORT --save "" --appendonly no --daemonize yes --dir ./ >/dev/null 2>&1
    sleep 2

    LOG_FILE="$OUT_DIR/res_${TRIGGERS}_${MODE}.log"

    # 3. 后台启动极限压测 
    # 🚀 核心修正：使用 SET，保持和你的 InazumaKV 对等的单层哈希寻址与负载大小！
    taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 1000000 -P $PIPELINE -r 1000000 SET bench_key:__rand_int__ val:__rand_int__ 2>/dev/null > $LOG_FILE &
    BENCH_PID=$!

    # 4. 核心逻辑：外部高频信号注入！
    if [ "$TRIGGERS" -gt 0 ]; then
        # 100万次请求在P=16下跑得极快，必须精密计算子弹发射间隔
        SLEEP_TIME=$(echo "scale=4; 1.0 / $TRIGGERS" | bc)
        
        for ((i=0; i<TRIGGERS; i++)); do
            # 如果压测已经提前结束了，就不再发了
            if ! kill -0 $BENCH_PID 2>/dev/null; then break; fi
            
            # 向 Redis 发送 SAVE 或 BGSAVE 指令
            redis-cli -p $PORT $MODE >/dev/null 2>&1
            
            # 极短休眠，控制发射频率
            sleep $SLEEP_TIME 2>/dev/null || sleep 0.001
        done
    fi

    # 5. 等待压测完全结束
    wait $BENCH_PID 2>/dev/null

    # 6. 提取 QPS 和最大延迟
    QPS=$(grep "requests per second" $LOG_FILE | awk '{print $1}')
    LATENCY=$(grep -E "100.00% <=" $LOG_FILE | tail -n 1 | awk '{print $3}')

    echo -e "   📊 \033[32m官方吞吐量: $QPS QPS\033[0m | \033[33m官方极限毛刺: ${LATENCY:-N/A} ms\033[0m\n"

    # 7. 安全关闭
    redis-cli -p $PORT shutdown 2>/dev/null
    sleep 1
done

echo "✅ 官方 Redis 全部测试完成！"