#!/bin/bash

# 确保脚本遇到错误时退出
set -e

# 要求使用 sudo 运行，因为要自动启停需要 AF_XDP 权限的 kvstore
if [ "$EUID" -ne 0 ]; then
  echo "❌ 请使用 sudo 权限运行此脚本 (例如: sudo ./tools/test_aof_impact.sh)"
  exit 1
fi

HOST="127.0.0.1"
PORT="6379"
CONF_FILE="conf/master.conf"
REQS=1000000
CLIENTS=50
REPORT_FILE="aof_impact_report.log"

echo "========================================================="
echo "  🚀 InazumaKV 自动化对比测试：AOF (io_uring) 性能损耗率"
echo "========================================================="

# 每次执行前清理旧的报告
> $REPORT_FILE

# 辅助函数：修改配置文件中的持久化模式
set_persistence_mode() {
    local mode=$1
    sed -i -E "s/^persistence_mode[[:space:]]*=.*/persistence_mode = $mode/" $CONF_FILE
}

# 辅助函数：安全关闭 Server
stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill -SIGINT $SERVER_PID > /dev/null 2>&1 || true
        wait $SERVER_PID 2>/dev/null || true
        sleep 1
    fi
}

# 确保退出时恢复环境，并清理临时文件
trap 'stop_server; set_persistence_mode "aof"; rm -f tmp_none.log tmp_aof.log; exit' INT TERM EXIT

# 清理历史数据
rm -rf data/*
pkill -9 kvstore 2>/dev/null || true
sleep 1


# ==========================================
# 阶段一：纯内存极限测试 (AOF 关闭)
# ==========================================
echo -e "\n[1/4] 🔧 阶段一：配置为纯内存模式 (persistence_mode = none)"
set_persistence_mode "none"

echo "[2/4] 启动 InazumaKV (后台运行) 并等待就绪..."
./bin/kvstore $CONF_FILE > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

echo "      ▶ 正在注入 $REQS 条离散写指令，测试极限 QPS (下方为实时进度)..."
echo "---------------------------------------------------------"
# 用 HSET 替代 SET 避免 O(N^2) 线性去重惩罚，利用 tee 抓取实时输出
redis-benchmark -h $HOST -p $PORT -c $CLIENTS -n $REQS -r 1000000 "HSET" "key:__rand_int__" "val:__rand_int__" | tee tmp_none.log
echo "---------------------------------------------------------"

# 从临时文件中提取最终的 QPS 数字
QPS_NONE=$(grep "requests per second" tmp_none.log | awk '{print $1}')

# 关闭服务器
stop_server
rm -rf data/*


# ==========================================
# 阶段二：io_uring 异步落盘测试 (AOF 开启)
# ==========================================
echo -e "\n[3/4] 🔧 阶段二：配置为异步落盘模式 (persistence_mode = aof)"
set_persistence_mode "aof"

echo "[4/4] 重新启动 InazumaKV (后台运行) 并等待就绪..."
./bin/kvstore $CONF_FILE > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

echo "      ▶ 正在注入同样的 $REQS 条指令，测试带磁盘 I/O 的 QPS..."
echo "---------------------------------------------------------"
redis-benchmark -h $HOST -p $PORT -c $CLIENTS -n $REQS -r 1000000 "HSET" "key:__rand_int__" "val:__rand_int__" | tee tmp_aof.log
echo "---------------------------------------------------------"

# 提取 QPS 数字
QPS_AOF=$(grep "requests per second" tmp_aof.log | awk '{print $1}')

# 关闭服务器
stop_server

# ==========================================
# 阶段三：计算与输出最终报告 (保存至 Log 文件)
# ==========================================
# 容错处理：如果 QPS 没有抓到，给个默认值防止报错
if [ -z "$QPS_NONE" ] || [ -z "$QPS_AOF" ]; then
    echo "❌ 压测数据抓取失败，请检查 redis-benchmark 是否正常运行！"
    exit 1
fi

# 使用 awk 进行浮点数计算损耗百分比: (1 - AOF/NONE) * 100
DROP_PERCENT=$(awk "BEGIN {printf \"%.2f\", (1 - $QPS_AOF / $QPS_NONE) * 100}")

# [核心修改] 将最终结果包裹在 {} 中，并通过 tee 输出到文件与屏幕
{
echo -e "\n========================================================="
echo "  📊 最终性能对比报告 (AOF 开启前后)"
echo "========================================================="
echo "  🚀 纯内存物理极限 (AOF OFF) : $QPS_NONE QPS"
echo "  🛡️ io_uring 落盘   (AOF ON)  : $QPS_AOF QPS"
echo "  📉 性能损耗率                : $DROP_PERCENT %"
echo "========================================================="
} | tee -a $REPORT_FILE

echo -e "\n✅ 详细报告保存至当前目录下的: \033[32m$REPORT_FILE\033[0m"

# 正常退出前的清理
trap - INT TERM EXIT
set_persistence_mode "aof"
rm -f tmp_none.log tmp_aof.log