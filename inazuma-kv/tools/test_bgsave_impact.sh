#!/bin/bash

set -e

HOST="127.0.0.1"
PORT="6379"

echo "========================================================="
echo "  🚀 InazumaKV 对照测试：后台 BGSAVE (COW) 无损性能分析"
echo "========================================================="

# 1. 注入 100 万级基准数据 (如果刚才没清空，这步其实可以跳过，但为了严谨我们再打一次)
echo "[1/3] 正在使用 HSET 注入 1,000,000 条基准离散数据..."
redis-benchmark -h $HOST -p $PORT -c 50 -n 1000000 -r 1000000 "HSET" "key:__rand_int__" "val:__rand_int__" -q

# 2. 启动后台持续高频探测
echo "[2/3] 数据注入完毕。启动后台低负载持续读探针..."
redis-benchmark -h $HOST -p $PORT -c 1 -n 500000 -r 1000000 "GET" "key:__rand_int__" > bgsave_impact_report.log &
BENCH_PID=$!

sleep 1

# 3. 实施对比打击：后台异步落盘
echo "[3/3] 🛡️ 实施对比：主线程触发异步 BGSAVE！"
echo "  -> BGSAVE 开始时间: $(date +%H:%M:%S.%3N)"

# time 命令现在测出的是 fork() 系统调用的耗时，应该极其短暂
time redis-cli -h $HOST -p $PORT BGSAVE

echo "  -> BGSAVE 触发完毕，子进程正在后台默默落盘..."

echo "[等待] 正在收集后台探针的尾盘数据..."
wait $BENCH_PID

echo "========================================================="
echo "  ✅ 测试完成！请查阅当前目录下的 bgsave_impact_report.log"
echo "  核心关注点：请查看日志最下方的 'Max Latency' (最大延迟)"
echo "========================================================="