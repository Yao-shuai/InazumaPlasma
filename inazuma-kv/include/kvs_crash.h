/**
 * @file        kvs_crash.h
 * @brief       InazumaKV 崩溃拦截与调用栈追踪模块头文件
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        提供企业级的 "黑匣子" (Crash Dump) 保护机制声明。
 * 暴露引擎致命错误拦截器的初始化接口，用于在发生不可恢复的内存或硬件级异常时
 * 捕获并输出底层的函数调用栈 (Call Stack)，防止脏数据落盘。
 */

#ifndef __KVS_CRASH_H__
#define __KVS_CRASH_H__

/* ==========================================================================
 * 对外暴露的守护层 API (Public Guard APIs)
 * ========================================================================== */

/**
 * @brief       初始化崩溃守护机制 (挂载致命错误拦截器)
 * @note        只需要在引擎启动时调用一次 (建议在 main 函数的最顶端优先调用)。
 * 成功挂载后，将静默守护整个进程的生命周期，接管 SIGSEGV、SIGABRT 等致命内核信号。
 */
void kvs_crash_guard_init(void);

#endif // __KVS_CRASH_H__