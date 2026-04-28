/**
 * @file        xdp_gateway.c
 * @brief       独立的高性能 AF_XDP 旁路网关入口进程
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        采用 AF_XDP (eXpress Data Path) 技术实现纯内存驱动的网络数据包收发。
 * 彻底绕过操作系统内核 TCP/IP 协议栈 (Kernel Bypass)，且规避了传统 Socket IPC 开销。
 * 作为独立的网关守护进程运行，通过无锁共享内存环形队列 (shm_ipc) 与 KVS 核心引擎进行极速数据交换。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "xdp_module.h"
#include "shm_ipc.h"

/* ==========================================================================
 * 全局运行状态控制 (Global Run-State Control)
 * ========================================================================== */

/** * @brief 全局运行标志位，采用 volatile 防止编译器优化，用于安全终止轮询死循环 
 */
volatile int g_keep_running = 1;


/* ==========================================================================
 * 中断与信号处理 (Interrupt & Signal Handling)
 * ========================================================================== */

/**
 * @brief       系统信号拦截例程 (Signal Handler)
 * @param[in]   signal 捕获到的系统信号编号
 * @note        拦截 SIGINT (Ctrl+C) 和 SIGTERM，触发全局退出标志，
 * 保证 UMEM 内存池和 XDP BPF 程序的安全卸载，避免遗留脏状态导致网卡挂死。
 */
void sigint_handler(int signal) {
    if (g_keep_running) {
        printf("\n[Gateway] Caught signal %d. Shutting down gracefully...\n", signal);
        g_keep_running = 0;
        
        /* 执行底层 AF_XDP 资源清理与网卡状态重置 */
        xdp_cleanup();
        exit(0);
    }
}


/* ==========================================================================
 * 主进程入口 (Main Process Entry)
 * ========================================================================== */

/**
 * @brief       AF_XDP 旁路网关守护进程入口
 * @param[in]   argc 命令行参数数量
 * @param[in]   argv 命令行参数字符串数组
 * @return      int  0: 正常退出; -1: 初始化失败
 */
int main(int argc, char *argv[]) {
    /* 注册终端中断与系统终止信号 */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* 默认运行参数 */
    int target_queue = 0; 
    const char *ifname = "eth_xdp"; 
    int is_master = 1; 
    const char *target_ip = "0.0.0.0"; 
    const char *target_mac = "00:00:00:00:00:00";

    /* 解析命令行挂载参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--queue") == 0 && i + 1 < argc) {
            target_queue = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--master") == 0) {
            is_master = 1;
        } else if (strcmp(argv[i], "--slave") == 0) {
            is_master = 0;
        } else if (strcmp(argv[i], "--target-ip") == 0 && i + 1 < argc) {
            target_ip = argv[++i];
        } else if (strcmp(argv[i], "--target-mac") == 0 && i + 1 < argc) {
            target_mac = argv[++i];
        } else if (strncmp(argv[i], "eth", 3) == 0 || strncmp(argv[i], "ens", 3) == 0) {
            ifname = argv[i];
        }
    }

    /* 终端审计日志输出 */
    printf("====================================================\n");
    printf(" [AF_XDP Gateway] Role: %s | Interface: %s\n", is_master ? "MASTER" : "SLAVE", ifname);
    printf("====================================================\n");

    /* * 阶段 1: 初始化 XDP 控制面
     * 包含 UMEM 内存池分配、BPF 字节码加载、AF_XDP 套接字 (XSK) 绑定。
     */
    if (xdp_init(ifname, target_queue, is_master, target_mac, target_ip) != 0) {
        fprintf(stderr, "[Fatal] XDP Init failed. Are you root? Is the interface correct?\n");
        return -1;
    }

    /* * 阶段 2: 数据面轮询死循环 (The Polling Hot-Loop)
     * 在此阶段 CPU 核心将被持续占满 (100%)，用于通过 NAPI 驱动直接无中断拉取网卡缓冲区数据。
     */
    xdp_poll_loop();
    
    /* * 阶段 3: 生命周期终止与优雅退出
     */
    xdp_cleanup();
    return 0;
}