#!/bin/bash

set -e

HOST="127.0.0.1"
PORT="6379"

if ! command -v pidstat &> /dev/null; then
    echo "❌ 找不到 pidstat 命令。请先执行: sudo apt-get install sysstat"
    exit 1
fi

echo "========================================================="
echo "  🚀 InazumaKV 极端测试：千万级 I/O 争抢与进程隔离分析"
echo "========================================================="

MAIN_PID=$(pgrep -x kvstore)
if [ -z "$MAIN_PID" ]; then
    echo "❌ 找不到 kvstore 进程，请先启动服务器！"
    exit 1
fi
echo "🎯 已锁定 InazumaKV 主进程 PID: $MAIN_PID"

# 【核心修改】将数据量提升至 1000 万，强制拉长落盘时间！
echo "[1/4] 正在注入 10,000,000 条数据 (这大概需要 1-2 分钟，请耐心等待)..."
redis-benchmark -h $HOST -p $PORT -c 50 -n 10000000 -r 10000000 "HSET" "key:__rand_int__" "val:__rand_int__" -q

# ==========================================
# 阶段一：同步 SAVE 的 I/O 监控
# ==========================================
echo -e "\n[2/4] 💥 实施核打击：正在执行同步 SAVE 并抓取 I/O 现场..."
> io_save.log
# 采集 5 次，每次 1 秒，确保能完整覆盖这几秒的落盘过程
LC_ALL=C pidstat -d 1 5 -C kvstore > io_save.log &
PIDSTAT_PID=$!

sleep 1 
echo "  -> SAVE 开始..."
redis-cli -h $HOST -p $PORT SAVE > /dev/null
echo "  -> SAVE 结束."
wait $PIDSTAT_PID 2>/dev/null || true

# ==========================================
# 阶段二：异步 BGSAVE 的 I/O 监控
# ==========================================
echo -e "\n[3/4] 🛡️ 实施对比实验：正在执行异步 BGSAVE 并抓取 I/O 现场..."
> io_bgsave.log
LC_ALL=C pidstat -d 1 5 -C kvstore > io_bgsave.log &
PIDSTAT_PID=$!

sleep 1
echo "  -> BGSAVE 开始..."
redis-cli -h $HOST -p $PORT BGSAVE > /dev/null
echo "  -> 主线程已返回，等待后台子进程落盘完成..."
wait $PIDSTAT_PID 2>/dev/null || true
echo "  -> BGSAVE 理论落盘完毕."

# ==========================================
# 阶段三：战报生成
# ==========================================
echo -e "\n========================================================="
echo "  📊 I/O 争抢测试结果汇报"
echo "========================================================="

echo -e "🔥 【灾难现场】同步 SAVE 期间的磁盘写入抓拍 (主进程 $MAIN_PID 亲自下场被卡死):"
awk '/kvstore/ { if ($5 > 0) print "   [PID: "$3"] 写入速度: "$5" KB/s" }' io_save.log || echo "  未抓取到有效 I/O 数据"

echo -e "\n🛡️ 【完美隔离】异步 BGSAVE 期间的磁盘写入抓拍 (主进程 $MAIN_PID 0写入，全由新 PID 承担):"
awk '/kvstore/ { if ($5 > 0) print "   [PID: "$3"] 写入速度: "$5" KB/s" }' io_bgsave.log || echo "  未抓取到有效 I/O 数据"
echo "========================================================="

rm -f io_save.log io_bgsave.log