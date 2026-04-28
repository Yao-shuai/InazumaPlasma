#!/bin/bash
# ==============================================================================
# Copyright (c) 2026 Nexus_Yao. All rights reserved.
#
# Project:      Inazuma AI Ecosystem
# File:         start_master.sh
# Description:  Master 节点自动化部署与启动脚本。
#               负责初始化运行环境、清理历史进程、启动底层 InazumaKV 存储节点，
#               并通过 Docker Compose 编排启动控制面业务微服务。
# ==============================================================================

echo "🚀 正在点火 Inazuma 混合架构全自动化集群 (Master 节点)..."

ROOT_DIR=$(pwd)
LOG_DIR="$ROOT_DIR/logs"
DATA_DIR="$ROOT_DIR/inazuma-kv/data"

# 创建所需的日志与数据持久化目录
mkdir -p "$LOG_DIR" "$DATA_DIR"

# 提权缓存，避免后续的 sudo 阻断自动化执行流程
sudo -v

# =========================================================================
# 1. 环境加载与残余进程清理
# =========================================================================
echo "🧹 正在清理 Master 历史环境与游离进程..."

# 加载全局环境变量
if [ -f "$ROOT_DIR/.env" ]; then
    set -a; source "$ROOT_DIR/.env"; set +a
else
    echo "⚠️ 警告: 未找到 .env 文件！"
fi

# 强制终止可能占用端口的底层组件进程
sudo killall -9 kvstore xdp_gateway redis-server 2>/dev/null

# =========================================================================
# 2. 启动底层数据面引擎
# =========================================================================
echo "📦 启动 InazumaKV 极速底座 (绑核 CPU:0, 端口: 6379)..."
cd "$ROOT_DIR/inazuma-kv"

# 使用 stdbuf -oL 调整标准输出为行缓冲模式
# 打破 C++ 默认的 4KB 块缓冲机制，确保底层日志实时刷入监控文件
sudo bash -c "taskset -c 0 stdbuf -oL ./bin/kvstore conf/master.conf > '$LOG_DIR/kv.log' 2>&1 & echo \$! > '$LOG_DIR/kv.pid'"

# 确保数据目录的宿主用户权限正确，忽略执行失败的情况
sudo chown -R $USER:$USER "$DATA_DIR" 2>/dev/null || true

# 等待存储引擎初始化就绪
sleep 3

# =========================================================================
# 3. 拉起控制面微服务集群
# =========================================================================
echo "🐳 正在拉起 Docker 控制面集群 (MySQL, Tokenizer, Gateway, Chat, Dashboard, Nginx)..."
cd "$ROOT_DIR"

# 附加 --build 参数强制重建镜像，确保 Python 微服务层的本地代码变更实时生效
sudo docker-compose up -d --build

# =========================================================================
# 4. 集群状态检查与标准作业程序 (SOP) 引导
# =========================================================================

# 动态获取宿主机物理 IP
# 策略：优先提取默认路由网卡的出口 IP；若提取失败，则退退为获取第一个非本地环回的 IP
HOST_IP=$(ip route get 1.1.1.1 2>/dev/null | grep -oP 'src \K\S+')
if [ -z "$HOST_IP" ]; then
    HOST_IP=$(hostname -I | awk '{print $1}')
fi

# 获取底层 KV 引擎进程号
KV_PID=$(cat "$LOG_DIR/kv.pid" 2>/dev/null)

echo -e "\n========================================================================"
echo "🎯 Inazuma Master 第一阶段 [TCP 基础及业务层] 点火完成！"
echo "========================================================================"
echo "📊 【集群运行状态】"
echo " 🟢 数据面 (Native): InazumaKV Master 已启动 (PID: ${KV_PID:-UNKNOWN}, 绑核: CPU 0)"
echo " 🐳 控制面 (Docker): 分词器(8001)、网关(8085)、计费、大屏、Nginx 已全部就绪"
echo ""
echo "🔗 【对外服务入口】"
echo " 📈 科技紫监控大屏 : http://$HOST_IP"
echo " 🤖 Chatbox 代理地址: http://$HOST_IP (请填入此地址的 /v1)"
echo "========================================================================"
echo "⚡ 【下一阶段：AF_XDP 旁路挂载与极限压测 SOP】"
echo " 请确保您始终在 ~/work/inazuma-ecosystem 根目录下执行以下命令："
echo ""
echo " 👉 步骤 1 [从机 a@1]: 建立 TCP 握手"
echo "    sudo taskset -c 0-3 ./inazuma-kv/bin/kvstore ./inazuma-kv/conf/slave.conf"
echo ""
echo " 👉 步骤 2 [从机 a@1]: 挂载 Slave 旁路网关"
echo "    sudo ./enable_xdp_slave.sh"
echo ""
echo " 👉 步骤 3 [主机本机]: 挂载 Master 旁路网关并开火压测！"
echo "    sudo ./enable_xdp_master.sh"
echo "========================================================================"