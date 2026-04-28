#!/bin/bash
# ============================================================================
# File: run_rdb_bench.sh
# Desc: 针对全量持久化 (SAVE vs BGSAVE) 真实高压冲击测试 (完美适配表格需求)
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

CMD=${1:-none}   # 接收参数：none, save, bgsave
TIMES=${2:-0}    # 接收参数：触发次数 (0, 1, 10)
PORT=6379
OUT_DIR="test_results/rdb_bench"
mkdir -p $OUT_DIR

# 1. 启动前清理环境，确保绝对干净
killall -9 kvstore redis-benchmark redis-cli 2>/dev/null
rm -f data/kvs.rdb data/kvs.aof
echo "🧹 [清理] 启动前已清空历史 kvs.rdb 文件。"

# 2. 生成 RDB 模式配置
cat <<EOF > $OUT_DIR/rdb.conf
bind_ip = 0.0.0.0
port = $PORT
log_level = warn
persistence_mode = rdb
ifname = "" 
EOF

# 3. 启动引擎 
# 🚀 绑在 CPU 0 和 3 上。主线程在 0，BGSAVE 的子进程会被 Linux 调度到 3，彻底物理隔离！
echo "🚀 启动 KV 引擎 (RDB 模式)..."
taskset -c 0,3 ./bin/kvstore $OUT_DIR/rdb.conf > $OUT_DIR/engine.log 2>&1 &
KV_PID=$!
sleep 3 

# 4. 💣 真实后台引爆器：养肥了再杀，保证连击有效！
if [ "$CMD" != "none" ] && [ "$TIMES" -gt 0 ]; then
    (
        # 🚀 等待 1.5 秒！让发包机先塞入几百万数据，把哈希表填满，生成巨无霸 RDB！
        sleep 1.5 
        for i in $(seq 1 $TIMES); do
            redis-cli -p $PORT $CMD > /dev/null 2>&1
            echo "💥 [System] 内存重压！精准触发 $CMD 强制落盘 ($i/$TIMES)" >> $OUT_DIR/trigger.log
            # 🚀 每隔 0.5 秒触发一次！确保上一次 BGSAVE 快写完了再触发下一次，防止被 C 代码防线拦截！
            sleep 0.5
        done
    ) &
fi

# 5. 启动终极极限压测
# 🚀 -n 10000000 和 -r 10000000：1000 万完全不同的海量数据，压测持续约 5-6 秒！
# 🚀 -P 16：确保 Pipeline 批量发送开启，榨干 QPS！
# 🚀 taskset -c 1,2：把压测工具赶到别的 CPU，防止抢占引擎算力！
echo "⚔️ 启动 10,000,000 条海量数据压测 (Pipeline: 16)..."
taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 10000000 -r 10000000 -P 16 HSET rdb_key:__rand_int__ rdb_val:__rand_int__ 2>/dev/null | tee $OUT_DIR/bench_${CMD}_${TIMES}.txt

# 6. 安全关闭
kill -SIGINT $KV_PID 2>/dev/null
sleep 2

# 7. 打印成绩单与文件大小
echo -e "\n📊 ========== 成绩单 ($CMD - $TIMES 次) =========="
grep "requests per second" $OUT_DIR/bench_${CMD}_${TIMES}.txt
grep -E "100.00%" $OUT_DIR/bench_${CMD}_${TIMES}.txt
ls -lh data/kvs.rdb 2>/dev/null | awk '{print "📁 验证真实落盘文件: " $9 " | 大小: " $5}'

# # 8. 彻底销毁证据
# rm -f data/kvs.rdb
# echo "🧹 [清理] 压测结束，已彻底删除生成的 kvs.rdb 文件！"
# echo -e "====================================================\n"
# sleep 3