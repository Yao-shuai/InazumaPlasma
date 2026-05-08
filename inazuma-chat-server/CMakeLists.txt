# ==============================================================================
# Copyright (c) 2026 Nexus_Yao. All rights reserved.
#
# Project:      InazumaChatServer
# File:         CMakeLists.txt
# Description:  聊天服务端模块的构建脚本。
#               配置 C++17 编译规范，并管理 OpenSSL、MySQL、jwt-cpp 等外部
#               依赖项的包含与链接。
# ==============================================================================

# 设置最低 CMake 版本要求，以确保构建指令的兼容性
cmake_minimum_required(VERSION 3.10)

# 定义项目名称及当前版本号
project(InazumaChatServer VERSION 1.0)

# 指定 C++ 编译标准为 C++17，并强制编译器严格遵循该标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 查找并加载系统级 OpenSSL 依赖库
# REQUIRED 关键字确保在未找到 OpenSSL 时立即终止 CMake 构建过程
find_package(OpenSSL REQUIRED)

# 配置全局头文件搜索路径
# 1. 包含项目自身的头文件目录
include_directories(${PROJECT_SOURCE_DIR}/include)
# 2. 包含本地集成的第三方库 jwt-cpp 头文件目录
include_directories(${PROJECT_SOURCE_DIR}/include/jwt-cpp/include)
# 3. 包含系统级 MySQL 客户端驱动头文件目录
include_directories(/usr/include/mysql)

# 扫描 src 目录下的所有 .cpp 源码文件并存入 SOURCES 变量
# 注：工程实践中若源文件发生增删，需手动重新运行 CMake 以更新 GLOB 缓存
file(GLOB SOURCES "src/*.cpp")

# 声明编译目标：利用扫描到的源码文件生成 chat_server 可执行程序
add_executable(chat_server ${SOURCES})

# 配置可执行目标的动态/静态链接库依赖
# 包括 MySQL 客户端底层驱动 (mysqlclient) 以及 OpenSSL 的安全传输 (SSL) 与密码学 (Crypto) 组件
target_link_libraries(chat_server mysqlclient OpenSSL::SSL OpenSSL::Crypto)

# 规范化输出路径：将编译生成的可执行文件统一输出到项目根目录的 bin 文件夹下
set_target_properties(chat_server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)