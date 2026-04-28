#!/bin/bash
# ==============================================================================
# Copyright (c) 2026 Nexus_Yao. All rights reserved.
#
# Project:      Inazuma AI Ecosystem
# File:         stop_ecosystem.sh
# Description:  集群环境安全停机脚本。
#               负责优雅关闭底层 C++ 存储引擎与 XDP 旁路网关，停止 Docker 控制面，
#               强制回收被占用的网络端口，并重置终端 TTY 状态。
# ==============================================================================

# 定义终端输出的 ANSI 颜色常量
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${BLUE}>>> [STOP] Initiating Inazuma Ecosystem Shutdown Sequence...${NC}"

ROOT_DIR=$(pwd)
LOG_DIR="$ROOT_DIR/logs"

# =========================================================================
# 1. 终止物理机原生进程 (InazumaKV 与 XDP 旁路网关)
# =========================================================================
for service in kv xdp_gateway; do
    if [ -f "$LOG_DIR/${service}.pid" ]; then
        PID=$(cat "$LOG_DIR/${service}.pid")
        echo -e "${YELLOW}--- [1/3] Terminating ${service} (PID: $PID) gracefully...${NC}"
        
        # 发送 SIGTERM (15) 信号，触发进程内部的析构逻辑 (如 eBPF 卸载、数据刷盘)
        sudo kill -15 $PID 2>/dev/null
        sleep 1.5
        
        # 发送 SIGKILL (9) 信号作为兜底策略，清理未响应的进程
        sudo kill -9 $PID 2>/dev/null
        sudo rm -f "$LOG_DIR/${service}.pid"
    fi
done

# =========================================================================
# 2. 终止 Docker 容器编排集群
# =========================================================================
echo -e "${YELLOW}--- [2/3] Stopping Docker Control Plane (Please wait)...${NC}"
cd "$ROOT_DIR"

# 屏蔽 docker-compose 的标准输出与错误输出，避免干扰主控台日志
sudo docker-compose stop > /dev/null 2>&1

# =========================================================================
# 3. 强制资源回收与端口释放
# =========================================================================
echo -e "${YELLOW}--- [3/3] Force cleaning orphaned processes and freeing ports...${NC}"

# 基于进程名执行全局清理，防止 PID 文件丢失导致的遗漏
sudo pkill -9 -f "kvstore" 2>/dev/null
sudo pkill -9 -f "xdp_gateway" 2>/dev/null

# 静默释放核心微服务绑定的 TCP 端口
sudo fuser -k 8080/tcp 8081/tcp 6379/tcp 8001/tcp 8085/tcp > /dev/null 2>&1

echo -e "${GREEN}>>> [SUCCESS] All resources released. System is safely offline.${NC}"

# =========================================================================
# 4. 终端状态重置
# =========================================================================
# 恢复 TTY 的默认显示属性，清除 ANSI 颜色残留与可能引起的键盘回显异常
stty sane
tput sgr0