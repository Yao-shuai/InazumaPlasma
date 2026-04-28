#!/bin/bash

# 确保脚本遇到错误时退出
set -e

# 1. 检查权限
if [ "$EUID" -ne 0 ]; then
  echo "❌ 请使用 sudo 权限运行此脚本 (因为 perf 抓取底层堆栈需要 Root 权限)"
  exit 1
fi

# 2. 配置 FlameGraph 工具路径 (请确认你之前把它 clone 到了这里)
# 如果你放在了其他地方，请修改这个路径！
FLAMEGRAPH_DIR="/home/$SUDO_USER/FlameGraph"

if [ ! -f "$FLAMEGRAPH_DIR/flamegraph.pl" ]; then
    echo "❌ 找不到 FlameGraph 工具，请检查 FLAMEGRAPH_DIR 路径是否正确: $FLAMEGRAPH_DIR"
    exit 1
fi

HOST="127.0.0.1"
PORT="6379"
CONF_FILE="conf/master.conf"
SVG_FILE="inazumakv_cpu_flamegraph.svg"

echo "========================================================="
echo "  🔥 InazumaKV CPU 火焰图一键生成器"
echo "========================================================="

# 清理旧环境
rm -rf data/*
pkill -9 kvstore 2>/dev/null || true
sleep 1

# 3. 启动 InazumaKV (配置为开启 AOF，抓取 I/O 和计算的热点)
echo "[1/5] 启动 InazumaKV 并开启 AOF 落盘机制..."
sed -i -E "s/^persistence_mode[[:space:]]*=.*/persistence_mode = aof/" $CONF_FILE
./bin/kvstore $CONF_FILE > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

# 4. 在后台启动压测打流 (持续施加压力，保证 perf 能抓到数据)
echo "[2/5] 启动后台高并发写流量 (制造 CPU 燃烧现场)..."
redis-benchmark -h $HOST -p $PORT -c 50 -n 5000000 -r 1000000 "HSET" "key:__rand_int__" "val:__rand_int__" -q > /dev/null 2>&1 &
BENCH_PID=$!
sleep 1 # 等待流量稳定

# 5. 使用 perf 抓取 CPU 堆栈
echo "[3/5] 📸 正在使用 perf 抓取进程 (PID: $SERVER_PID) 的堆栈信息 (耗时 10 秒)..."
# -F 99: 每秒采样99次
# -p: 指定进程PID
# -g: 抓取调用关系树 (call-graph)
# -- sleep 10: 抓取持续 10 秒
perf record -F 99 -p $SERVER_PID -g -- sleep 10

# 6. 生成火焰图
echo "[4/5] 🎨 正在渲染 SVG 火焰图..."
perf script > out.perf
$FLAMEGRAPH_DIR/stackcollapse-perf.pl out.perf > out.folded
$FLAMEGRAPH_DIR/flamegraph.pl out.folded > $SVG_FILE

# 7. 打扫战场
echo "[5/5] 测试完毕，正在清理压测进程和临时文件..."
kill -9 $BENCH_PID 2>/dev/null || true
kill -SIGINT $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
rm -f out.perf out.folded perf.data

echo "========================================================="
echo "  ✅ 成功！你的专属火焰图已生成："
echo "  👉 路径: $(pwd)/$SVG_FILE"
echo "  💡 提示: 请用 Chrome、Edge 等现代浏览器直接打开该 .svg 文件！"
echo "========================================================="