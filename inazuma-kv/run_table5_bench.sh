#!/bin/bash
# ============================================================================
# File: run_table5_bench.sh
# Desc: 自动修改 C 源码宏定义 -> 重新编译 -> 压测 -> 收集全量持久化冲击数据
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

PORT=6379
PIPELINE=16
OUT_DIR="test_results/table5_bench"
mkdir -p $OUT_DIR
killall -9 kvstore redis-benchmark 2>/dev/null

# 压测配置矩阵: (触发次数 | 写命令阈值 | BGSAVE模式 0=SAVE, 1=BGSAVE)
# 1000000次触发1次, 100000次触发10次, 10000次触发100次, 1000次触发1000次
CONFIGS=(
    "0_0_0"         # 基准(关闭)
    "1_1000000_0"   # SAVE 1次
    "10_100000_0"   # SAVE 10次
    "100_10000_0"   # SAVE 100次
    "1000_1000_0"   # SAVE 1000次
    "1_1000000_1"   # BGSAVE 1次
    "10_100000_1"   # BGSAVE 10次
    "100_10000_1"   # BGSAVE 100次
    "1000_1000_1"   # BGSAVE 1000次
)

echo -e "================================================================"
echo -e " 🏆 InazumaKV 全量持久化 (SAVE / BGSAVE) 自动化打榜工具"
echo -e "================================================================\n"

for conf in "${CONFIGS[@]}"; do
    IFS='_' read -r TRIGGERS THRESHOLD USE_BG <<< "$conf"
    
    MODE_STR="SAVE (同步阻塞)"
    if [ "$USE_BG" == "1" ]; then MODE_STR="BGSAVE (异步Fork)"; fi
    if [ "$THRESHOLD" == "0" ]; then MODE_STR="纯内存基准 (无干扰)"; TRIGGERS="0"; fi

    echo -e "⚙️ 正在配置内核: \033[36m$MODE_STR | 触发 $TRIGGERS 次 (每 $THRESHOLD 条写入)\033[0m"

    # 1. 动态注入 C 语言宏定义
    sed -i -E "s/#define AUTO_SAVE_THRESHOLD [0-9]+/#define AUTO_SAVE_THRESHOLD $THRESHOLD/g" src/kvstore.c
    sed -i -E "s/#define USE_BGSAVE_FOR_AUTO [0-9]+/#define USE_BGSAVE_FOR_AUTO $USE_BG/g" src/kvstore.c

    # 2. 静默重新编译
    make clean >/dev/null 2>&1
    make >/dev/null 2>&1
    if [ $? -ne 0 ]; then echo "❌ 编译失败！"; exit 1; fi

    # 3. 物理清场
    rm -f /tmp/kvs.rdb /tmp/kvs.aof
    
    # 4. 生成临时配置并启动引擎
    cat <<EOF > $OUT_DIR/tmp.conf
bind_ip = 0.0.0.0
role = master
port = $PORT
log_level = error
persistence_mode = none
rdb_path = /tmp/kvs.rdb
EOF
    taskset -c 0 ./bin/kvstore $OUT_DIR/tmp.conf >/dev/null 2>&1 &
    KV_PID=$!
    sleep 2

    # 5. 启动极限压测 (加入 -e 参数获取 Latency，并过滤关键信息)
    LOG_FILE="$OUT_DIR/res_${TRIGGERS}_${USE_BG}.log"
    taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 1000000 -P $PIPELINE -r 1000000 HSET bench_key:__rand_int__ field:__rand_int__ val:__rand_int__ 2>/dev/null > $LOG_FILE

    # 6. 提取 QPS 和最大延迟
    QPS=$(grep "requests per second" $LOG_FILE | awk '{print $1}')
    # 最新版 benchmark 的延迟通常在 summary 里
    LATENCY=$(grep -E "100.00% <=" $LOG_FILE | tail -n 1 | awk '{print $3}')

    echo -e "   📊 \033[32m吞吐量: $QPS QPS\033[0m | \033[33m极限毛刺延迟: ${LATENCY:-N/A} ms\033[0m\n"

    # 7. 优雅关闭
    kill -SIGINT $KV_PID 2>/dev/null
    sleep 1
done

# 测试结束后恢复默认代码
sed -i -E "s/#define AUTO_SAVE_THRESHOLD [0-9]+/#define AUTO_SAVE_THRESHOLD 0/g" src/kvstore.c
echo "✅ 全部测试完成！C 源码宏已恢复默认。"