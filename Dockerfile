# ==============================================================================
# Copyright (c) 2026 Nexus_Yao. All rights reserved.
#
# Project:      Inazuma AI Ecosystem
# File:         Dockerfile
# Description:  控制面服务容器构建脚本。
#               采用多阶段构建 (Multi-stage Build) 策略，分离编译环境与运行环境，
#               以最小化最终镜像体积并降低安全攻击面。
# ==============================================================================

# ==============================================================================
# 第一阶段：编译环境 (Builder Stage)
# ==============================================================================
FROM ubuntu:22.04 AS builder

# 禁用 apt 安装时的交互式提示
ENV DEBIAN_FRONTEND=noninteractive

# 更新软件包列表并安装 C++ 编译依赖链及相关第三方库的开发包 (Headers)
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libcurl4-openssl-dev \
    libmysqlclient-dev \
    libssl-dev \
    libhiredis-dev \
    zlib1g-dev \
    liburing-dev \
    libjemalloc-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# 执行项目编译，当前策略仅生成网关与聊天服务两个核心可执行文件
RUN make gateway && make chat


# ==============================================================================
# 第二阶段：运行环境 (Runtime Stage)
# ==============================================================================
FROM ubuntu:22.04

# 禁用 apt 安装时的交互式提示
ENV DEBIAN_FRONTEND=noninteractive

# 安装核心程序运行时所需的动态链接库 (Shared Libraries) 以及 Python 3 运行环境
RUN apt-get update && apt-get install -y \
    libcurl4 \
    libmysqlclient-dev \
    libssl-dev \
    libhiredis-dev \
    ca-certificates \
    liburing-dev \
    libjemalloc-dev \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# 更新本地 CA 证书池，确保请求远端 HTTPS (如 OpenAI/DeepSeek API) 时的 TLS 校验正常
RUN update-ca-certificates

# 安装 Python 微服务组件及前端面板依赖，配置国内镜像源以提升构建稳定性
RUN pip3 install --default-timeout=1000 -i https://pypi.tuna.tsinghua.edu.cn/simple \
    streamlit pymysql pandas plotly cryptography \
    fastapi uvicorn tiktoken pydantic sentence-transformers

WORKDIR /app

# 从 builder 阶段提取已编译好的二进制文件与配套配置文件至运行容器
COPY --from=builder /build/inazuma-ai-gateway/bin/ai_gateway ./inazuma-ai-gateway/bin/
COPY --from=builder /build/inazuma-ai-gateway/conf ./inazuma-ai-gateway/conf/
COPY --from=builder /build/inazuma-chat-server/bin/chat_server ./inazuma-chat-server/bin/

# 拷贝基于 Python 的微服务与前端代码包
COPY --from=builder /build/inazuma-dashboard ./inazuma-dashboard/
COPY --from=builder /build/tokenizer ./tokenizer/

# 初始化日志目录结构
RUN mkdir -p logs

# 启动指令留空，将由容器编排工具 (如 docker-compose.yml 的 command 字段) 动态接管