#!/bin/bash

set -e

HOST="127.0.0.1"
PORT="6379"

echo "========================================================="
echo "  🚀 InazumaKV 破坏性测试用例：同步 SAVE 阻塞效应分析"
echo "========================================================="

# 1. 注入 100 万级基准数据
echo "[1/3] 正在使用 HSET 注入 1,000,000 条基准离散数据 (造流中，请稍候)..."
# 【关键修复】使用自定义指令，强制生成 100万个不同的 Key！
redis-benchmark -h $HOST -p $PORT -c 50 -n 1000000 -r 1000000 "HSET" "key:__rand_int__" "val:__rand_int__" -q

# 2. 启动后台持续高频探测
echo "[2/3] 数据注入完毕。启动后台低负载持续读探针 (模拟真实业务)..."
# 使用自定义的随机 GET 指令，防止命中同一个 Key
redis-benchmark -h $HOST -p $PORT -c 1 -n 500000 -r 1000000 "GET" "key:__rand_int__" > save_impact_report.log &
BENCH_PID=$!

# 等待一秒，让探针平稳运行
sleep 1

# 3. 实施核打击：同步落盘
echo "[3/3] 💥 实施打击：主线程触发全量同步 SAVE！(这下一定会卡住了)"
echo "  -> SAVE 开始时间: $(date +%H:%M:%S.%3N)"

time redis-cli -h $HOST -p $PORT SAVE

echo "  -> SAVE 结束时间: $(date +%H:%M:%S.%3N)"

echo "[等待] 正在收集后台探针的尾盘数据..."
wait $BENCH_PID

echo "========================================================="
echo "  ✅ 测试完成！请查阅当前目录下的 save_impact_report.log"
echo "  核心关注点：请查看日志最下方的 'Max Latency' (最大延迟)"
echo "========================================================="