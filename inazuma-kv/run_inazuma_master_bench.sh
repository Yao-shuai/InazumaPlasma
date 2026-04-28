#!/bin/bash
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

PORT=6379
PIPELINES=(1 4 8 16 20 40 80 160)

echo "🧹 1. 清理 Master 环境..."
killall -9 kvstore redis-server redis-benchmark 2>/dev/null
rm -f data/*

echo "🚀 2. 启动 InazumaKV Master (绑定 CPU 0)..."
taskset -c 0 ./bin/kvstore conf/master.conf &
sleep 2 

echo -e "\n===================================================="
echo "⏳ [发令枪等待] Master 已就绪！"
echo "👉 请现在切换到 Slave 机器 (192.168.124.14) 执行启动命令。"
echo "===================================================="
read -p "🛑 确认 Slave 已经成功启动并连接后，请按 [Enter] 键开始疯狂压测..."

echo "⚔️ 3. 开启高压火力全覆盖测试..."
for P in "${PIPELINES[@]}"; do
    echo -e "\n===================================================="
    echo "🔥 [InazumaKV] 正在测试 Pipeline: $P ..."
    taskset -c 1,2 redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 1000000 -r 1000000 -P $P HSET repl_key:__rand_int__ repl_val:__rand_int__ 2>/dev/null | grep -E "requests per second|100.00%"
    sleep 1
done

echo -e "\n🧹 4. 测试结束，安全关闭 Master..."
killall -9 kvstore