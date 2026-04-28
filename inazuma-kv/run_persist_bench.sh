#!/bin/bash
# ============================================================================
# File: run_persist_bench.sh
# Desc: AOF vs RDB (SAVE / BGSAVE) 持久化性能极限对比压测脚本 (支持 Pipeline)
# ============================================================================
if [ "$EUID" -ne 0 ]; then echo "❌ 请使用 sudo 运行！"; exit 1; fi

REAL_USER=${SUDO_USER:-$USER}
USER_HOME=$(eval echo ~$REAL_USER)
FLAME_DIR="$USER_HOME/FlameGraph"

# 支持的模式: none, aof, bgsave, save
MODE=${1:-none}
# 🚀 核心修改 1：新增 Pipeline 深度参数，默认 16！
PIPELINE=${2:-16} 
PORT=6379
OUT_DIR="test_results/persist_$MODE"
CONF_FILE="$OUT_DIR/persist.conf"

# 映射引擎配置 (引擎只认 rdb，但我们通过脚本发送不同的指令)
ENGINE_MODE=$MODE
if [[ "$MODE" == "save" || "$MODE" == "bgsave" ]]; then
    ENGINE_MODE="rdb"
fi

mkdir -p $OUT_DIR
killall -9 kvstore redis-benchmark perf tail 2>/dev/null

# 🎯 【终极修复】：精准打击 data 目录下的持久化文件，确保每次压测都是 0 数据开局！
rm -f data/kvs.rdb data/kvs.aof data/kvs.aof.tmp
echo "🧹 [清理] 已清空历史持久化数据，确保内存绝对干净！"
sleep 1 

# 🚀 核心修改 2：终端 UI 显示当前 Pipeline 深度
echo -e "\n===================================================="
echo -e " 💾 InazumaKV 持久化性能压测 (模式: \033[32m$MODE\033[0m | Pipeline: \033[33m$PIPELINE\033[0m)"
echo -e "===================================================="

# 1. 动态生成配置文件
cat <<EOF > $CONF_FILE
bind_ip = 0.0.0.0
role = master
port = $PORT
log_level = warn
persistence_mode = $ENGINE_MODE
ifname = "" 
EOF

# 2. 启动 KV 引擎 (绑核 CPU 0)
echo "🚀 [1/4] 正在启动 KV 引擎 (底层配置为 $ENGINE_MODE 模式)..."
taskset -c 0 ./bin/kvstore $CONF_FILE > $OUT_DIR/engine.log 2>&1 &
KV_PID=$!

echo "⏳ 等待引擎完成网络初始化 (5秒)..."
sleep 5 

# 3. 压测中途的“炸弹”定时器
if [ "$MODE" == "bgsave" ]; then
    echo "⏱️ [System] 压测策略: 中途触发 BGSAVE (异步 Fork，无损快照)..."
    (sleep 5 && echo -e "\n💥💥💥 [System] 触发 BGSAVE！后台开始 Fork 落盘！💥💥💥\n" && redis-cli -p $PORT BGSAVE) &
elif [ "$MODE" == "save" ]; then
    echo "⏱️ [System] 压测策略: 中途触发 SAVE (同步阻塞，主线程地狱)..."
    (sleep 5 && echo -e "\n🛑🛑🛑 [System] 触发 SAVE！主线程被彻底挂起强制落盘！🛑🛑🛑\n" && redis-cli -p $PORT SAVE) &
fi

# 4. 后台启动 perf record 抓取 CPU 状态
echo "🔥 [2/4] 启动 CPU 性能采样 (抓取主线程 PID: $KV_PID)..."
perf record -F 99 -p $KV_PID -g -o $OUT_DIR/perf.data &
PERF_PID=$!
sleep 1 

# 5. 启动官方核弹压测
# 🚀 核心修改 3：注入 -P $PIPELINE 参数到发包机
echo "⚔️ [3/4] 启动 redis-benchmark 狂轰 1,000,000 条 HSET 指令 (-P $PIPELINE)..."
echo -e "\033[90m--- 压测实时进度 --- \033[0m"

redis-benchmark -h 127.0.0.1 -p $PORT -c 50 -n 1000000 -P $PIPELINE -r 1000000 HSET bench_key:__rand_int__ bench_val:__rand_int__ 2>/dev/null | tee $OUT_DIR/bench_result.txt

# 6. 停止抓图并生成 SVG
echo -e "\n✅ 压测执行完毕！正在停止抓取并渲染火焰图..."
kill -INT $PERF_PID 2>/dev/null
wait $PERF_PID 2>/dev/null 

echo "🎨 [4/4] 正在生成 $MODE 模式专属火焰图..."
perf script -i $OUT_DIR/perf.data | $FLAME_DIR/stackcollapse-perf.pl | $FLAME_DIR/flamegraph.pl > $OUT_DIR/flamegraph_persist_$MODE.svg

# 🚀 【核心修复 1】：优雅退出机制 (Graceful Shutdown)
echo -e "\n🛡️ 正在安全关闭 KV 引擎，等待内存数据排空落盘..."
kill -SIGINT $KV_PID 2>/dev/null
# 给引擎 3 秒钟时间去清空 io_uring 队列、执行 fsync 并关闭文件
sleep 3 

# 修复权限与清理
chown -R $REAL_USER:$REAL_USER $OUT_DIR 2>/dev/null
chmod 644 $OUT_DIR/* 2>/dev/null

echo -e "\n📊 \033[36m========== 最终成绩单 ($MODE) ==========\033[0m"
cat $OUT_DIR/bench_result.txt | grep "requests per second"

# 🚀 【核心修复 2】：落盘文件检查必须放在优雅退出之后，此时文件才完整！
ls -lh data/kvs.rdb data/kvs.aof 2>/dev/null | awk '{print "📁 落盘文件: " $9 " | 大小: " $5}'
echo -e "🔥 火焰图已保存至: \033[33m$OUT_DIR/flamegraph_persist_$MODE.svg\033[0m"
echo -e "====================================================\n"

sleep 2