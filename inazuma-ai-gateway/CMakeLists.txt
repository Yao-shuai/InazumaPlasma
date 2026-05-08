# ==============================================================================
# Copyright (c) 2026 Nexus_Yao. All rights reserved.
#
# Project:      InazumaAIGateway
# File:         CMakeLists.txt
# Description:  AI 网关服务的构建配置文件。
# ==============================================================================

cmake_minimum_required(VERSION 3.10)
project(InazumaAIGateway CXX) # 明确声明仅使用 C++ 语言

# 1. 全局配置：输出目录标准化 (Modern 方式)
# 将所有生成的可执行文件统一输出到根目录的 bin 文件夹
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# 2. 定义可执行文件目标
add_executable(ai_gateway src/ai_gateway.cpp)

# 3. 目标属性配置：编译标准
# 直接将 C++ 标准绑定到具体的目标上，而不是全局
set_target_properties(ai_gateway PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF # 禁用特定编译器的扩展 (如 gnu++17)，强制使用纯净的 c++17 标准
)

# 4. 目标属性配置：包含目录
# PRIVATE 关键字表明这些头文件仅 ai_gateway 自身编译时需要
target_include_directories(ai_gateway PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

# 5. 目标属性配置：编译选项 (替代全局 CMAKE_CXX_FLAGS)
# 为当前目标精细化配置编译参数，避免污染项目中可能引入的其他第三方库
target_compile_options(ai_gateway PRIVATE
    -O3
    -march=native       # ⚠️ 注意: 若 CI/CD 编译机与线上生产机架构不同，请移除此项
    -funroll-loops
    -Wall
    -Wextra             # 建议附加：开启比 -Wall 更严格的警告
    -Wpedantic          # 建议附加：检查是否符合 ISO C++ 标准
)

# 6. 目标属性配置：依赖链接
# 🚀 纯净链接：只保留底层网络、缓存、请求和数据库的必要组件
target_link_libraries(ai_gateway PRIVATE 
    pthread             # 线程支持
    curl                # 外部请求路由
    hiredis             # 缓存交互
    mysqlclient         # 数据持久化
    uring               # 异步 I/O (io_uring)
)