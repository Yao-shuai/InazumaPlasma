/**
 * @file        mem_probe.h
 * @brief       InazumaKV 全局内存遥测与探针模块头文件
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        提供企业级内存分配状态监控与遥测的外部调用接口。
 * 涵盖探针的初始化、内存分配/释放的钩子 (Hooks) 声明，
 * 以及兼容标准 RESP 协议的监控遥测报文生成机制。
 */

#ifndef MEM_PROBE_H
#define MEM_PROBE_H

#include <stddef.h>

/* ==========================================================================
 * 生命周期与探测器控制 (Lifecycle & Probe Control)
 * ========================================================================== */

/**
 * @brief       初始化全局内存探针模块
 * @param[in]   enable 探针的激活标志位 (底层实现可能为保障强可观测性而强制覆写开启)
 * @note        负责绑定进程级别的内核中断信号 (如 SIGUSR1) 以触发控制台维度的监控面板。
 */
void mem_probe_init(int enable);


/* ==========================================================================
 * 内存操作埋点钩子 (Memory Allocation Hooks)
 * ========================================================================== */

/**
 * @brief       记录内存分配事件，驱动遥测原子指标累加
 * @param[in]   size 底层内存分配器 (如 jemalloc/glibc) 单次分配申请的真实字节数
 */
void mem_probe_alloc(size_t size);

/**
 * @brief       记录内存释放事件，驱动遥测原子指标递减
 * @param[in]   size 底层内存分配器单次回收的真实字节数
 */
void mem_probe_free(size_t size);


/* ==========================================================================
 * 监控指标与诊断数据导出 (Metrics Export & Diagnostics)
 * ========================================================================== */

/**
 * @brief       生成兼容标准 RESP 协议的 INFO MEMORY 遥测报文
 * @param[out]  buffer   用于承接格式化文本数据流的预分配缓冲区指针
 * @param[in]   max_len  缓冲区的最大安全尺寸边界，防止协议序列化时发生内存越界
 * @return      int      实际写入缓冲区的有效字节数
 * @note        输出格式遵循标准的 Key:Value 文本域范式，直接对标 Redis INFO 命令体系。
 * 核心暴露指标包含逻辑/物理内存水位、RSS 驻留率及操作系统级系统调用频率。
 */
int mem_probe_generate_info_memory(char *buffer, size_t max_len);

#endif // MEM_PROBE_H