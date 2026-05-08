#!/bin/bash
# ============================================================================
# File: run_inazuma_table5_external.sh
# Desc: InazumaKV 全量持久化 (SAVE / BGSAVE) 外部高频注入打榜脚本 (绝对公平对齐版)
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

PORT=6379
PIPELINE=16
OUT_DIR="test_results/inazuma_table5"
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
echo -e " 🏆 InazumaKV 全量持久化 (SAVE / BGSAVE) 外部注入自动化测试"
echo -e "================================================================\n"

for conf in "${CONFIGS[@]}"; do
    IFS='_' read -r TRIGGERS MODE <<< "$conf"
    
    echo -e "⚙️ 正在拷问 InazumaKV: \033[36m模式: $MODE | 外部强行触发 $TRIGGERS 次\033[0m"

    # 1. 启动前清理环境，确保绝对干净 (严格照搬你的成功代码)
    killall -9 kvstore redis-benchmark redis-cli 2>/dev/null
    rm -f data/kvs.rdb data/kvs.aof
    sleep 1

    # 2. 生成 RDB 模式配置 (严格照搬你的成功代码，使用原生路径)
    cat <<EOF > $OUT_DIR/rdb.conf
bind_ip = 0.0.0.0
port = $PORT
log_level = warn
persistence_mode = rdb
ifname = "" 
EOF

    LOG_FILE="$OUT_DIR/res_${TRIGGERS}_${MODE}.log"

    # 3. 启动引擎 (严格照搬你的绑核与启动逻辑)
    taskset -c 0,3 ./bin/kvstore $OUT_DIR/rdb.conf > $OUT_DIR/engine_${TRIGGERS}_${MODE}.log 2>&1 &
    KV_PID=$!
    sleep 3 

    # 4. 后台启动极限压测 (与官方 Redis 测试命令保持完全对等)
    taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 10000000 -P $PIPELINE -r 10000000 HSET bench_key:__rand_int__ field:__rand_int__ val:__rand_int__ 2>/dev/null > $LOG_FILE &
    BENCH_PID=$!

    # 5. 核心逻辑：外部高频信号注入！(完全复刻官方 Redis 的注入逻辑)
    if [ "$TRIGGERS" -gt 0 ]; then
        SLEEP_TIME=$(echo "scale=4; 1.0 / $TRIGGERS" | bc)
        
        for ((i=0; i<TRIGGERS; i++)); do
            # 如果压测已经提前结束了，就不再发了
            if ! kill -0 $BENCH_PID 2>/dev/null; then break; fi
            
            # 过滤掉 NONE 模式
            if [ "$MODE" != "NONE" ]; then
                redis-cli -p $PORT $MODE >/dev/null 2>&1 &
            fi
            
            # 极短休眠，控制发射频率
            sleep $SLEEP_TIME 2>/dev/null || sleep 0.001
        done
    fi

    # 6. 等待压测完全结束
    wait $BENCH_PID 2>/dev/null

    # 7. 提取 QPS 和最大延迟
    QPS=$(grep "requests per second" $LOG_FILE | awk '{print $1}')
    LATENCY=$(grep -E "100.00% <=" $LOG_FILE | tail -n 1 | awk '{print $3}')

    echo -e "   📊 \033[32mInazumaKV 吞吐量: ${QPS:-ERROR} QPS\033[0m | \033[33m极限毛刺: ${LATENCY:-N/A} ms\033[0m\n"

    # 8. 优雅关闭
    kill -SIGINT $KV_PID 2>/dev/null
    sleep 2
done

echo "✅ InazumaKV 外部注入打榜全部完成！"