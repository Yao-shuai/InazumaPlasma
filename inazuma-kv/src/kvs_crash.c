/**
 * @file        kvs_crash.c
 * @brief       InazumaKV 崩溃拦截与调用栈追踪模块 (Crash Dump)
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        提供企业级的 "黑匣子" 保护机制。当引擎遭遇段错误、堆损坏等致命异常时，
 * 抓取崩溃瞬间的调用栈并输出到标准错误，随后安全终止进程，防止脏数据落盘。
 */

#include "kvs_crash.h"
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * 内部核心拦截器 (Private Crash Handlers)
 * ========================================================================== */

/**
 * @brief       核心崩溃信号拦截处理函数 (Async-Signal-Safe)
 * @param[in]   sig  触发崩溃的系统信号编号 (例如 SIGSEGV)
 * @warning     [极度危险] 信号处理上下文中，严禁调用任何非异步信号安全 
 * (Non-Async-Signal-Safe) 的函数 (如 malloc, snprintf, syslog)。
 * 若堆内存已损坏，再次调用 malloc 将导致死锁或二次崩溃。
 */
static void crash_handler(int sig) {
    void *callstack[64];
    int frames;

    // 1. 拦截死神：利用 glibc 抓取崩溃瞬间的函数调用栈 (最多抓取 64 层)
    frames = backtrace(callstack, 64);

    // 2. 打印最醒目的空难警报 (使用 fprintf 直接写 stderr，相对安全)
    fprintf(stderr, "\n==================================================\n");
    fprintf(stderr, " 💥 [FATAL ERROR] InazumaKV Engine Crashed!\n");
    fprintf(stderr, " 💥 Caught fatal signal %d (%s)\n", sig, strsignal(sig));
    fprintf(stderr, "==================================================\n");
    fprintf(stderr, " 🛠️ Crash Backtrace (Call Stack):\n");

    // 3. 异步信号安全打印：直接向 STDERR_FILENO 写入栈帧符号，彻底杜绝 malloc
    backtrace_symbols_fd(callstack, frames, STDERR_FILENO);

    fprintf(stderr, "==================================================\n");
    fprintf(stderr, " 🚨 Engine is completely dead. Please report this stack to developers.\n");
    
    // 4. 彻底终止进程。绝对不要尝试抢救或恢复状态，防止错误数据污染内存与磁盘文件！
    exit(EXIT_FAILURE); 
}


/* ==========================================================================
 * 对外暴露接口 (Public APIs)
 * ========================================================================== */

/**
 * @brief       初始化崩溃守护机制 (一键开启全面守护)
 * @note        将核心致命信号绑定至 crash_handler 拦截器。
 * 建议在 main 函数的最顶端优先调用，确保引擎启动初期的错误也能被捕捉。
 */
void kvs_crash_guard_init(void) {
    // 拦截段错误 (非法内存访问，最常见的越界/野指针解引用)
    signal(SIGSEGV, crash_handler); 
    
    // 拦截 abort (常用于 assert() 失败，或 glibc 内部检测到 double-free 堆损坏)
    signal(SIGABRT, crash_handler); 
    
    // 拦截非法指令 (CPU 遇到无法解析的机器码，通常是函数指针跑飞)
    signal(SIGILL,  crash_handler); 
    
    // 拦截浮点异常 (如极其经典的整数除以 0 错误)
    signal(SIGFPE,  crash_handler); 
    
    // 拦截总线错误 (如物理内存错误，或 ARM 架构下非对齐的内存访问)
    signal(SIGBUS,  crash_handler); 
    
    // [预留日志接口] 保持静默启动，或记录一条启动成功的审计日志
    // printf("[SysGuard] 🛡️ Crash Dump Blackbox activated. Engine is protected.\n");
}