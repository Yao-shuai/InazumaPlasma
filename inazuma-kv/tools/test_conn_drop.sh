#!/bin/bash

set -e

HOST="127.0.0.1"
PORT="6379"
LOG_FILE="conn_drop_report.log"

echo "========================================================="
echo "  🚀 InazumaKV 极端测试：新建连接阻断 (Connection Drop)"
echo "========================================================="

echo "[1/4] 正在注入 1,000,000 条数据，为 SAVE 制造负担..."
redis-benchmark -h $HOST -p $PORT -c 50 -n 1000000 -r 1000000 "HSET" "key:__rand_int__" "val:__rand_int__" -q

> $LOG_FILE

echo "[2/4] 启动高频并发探针：模拟海量新用户涌入 (异步狂暴模式)..."
# 【关键修改】把 redis-cli 放到后台 (&) 执行，并且把 timeout 缩短到 20ms
# 这样探针就不会被卡住，而是会像加特林一样持续开火！
(
    while true; do
        timeout 0.02 redis-cli -h $HOST -p $PORT PING > /dev/null 2>&1 || echo "$(date +%H:%M:%S.%3N) ❌ 致命错误: 新客户端连接超时！" >> $LOG_FILE &
        sleep 0.005 # 每 5 毫秒开一枪
    done
) 2>/dev/null &  
PROBER_PID=$!

sleep 1

echo "[3/4] 💥 实施核打击：主线程触发全量同步 SAVE！"
echo "  -> SAVE 开始时间: $(date +%H:%M:%S.%3N)"

redis-cli -h $HOST -p $PORT SAVE > /dev/null

echo "  -> SAVE 结束时间: $(date +%H:%M:%S.%3N)"

sleep 0.5
# 【关键修改】优雅地干掉探针，防止打印“已杀死”
kill $PROBER_PID 2>/dev/null || true
wait $PROBER_PID 2>/dev/null || true

echo "[4/4] 正在分析连接阻断报告..."
FAIL_COUNT=$(grep "❌" $LOG_FILE | wc -l)

echo -e "\n========================================================="
echo "  📊 阻断测试结果汇报"
echo "========================================================="
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "  🚨 \033[31m发现阻断！在 SAVE 的 100 多毫秒内，共有 $FAIL_COUNT 个新客户端被拒之门外！\033[0m"
else
    echo "  ✅ 未发现连接失败。"
fi
echo "========================================================="