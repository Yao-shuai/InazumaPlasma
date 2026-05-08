/**
 * @file        shm_ipc.h
 * @brief       InazumaKV 跨进程共享内存与无锁 SPSC 环形队列头文件
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        提供基于 POSIX 共享内存的极速 IPC (Inter-Process Communication) 机制。
 * 针对现代 CPU 缓存架构 (L1/L2/L3 Cache) 进行了极限对齐优化，
 * 彻底消除高频并发下的缓存伪共享 (False Sharing) 问题。
 */

#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdalign.h>

/* ==========================================================================
 * 宏定义与容量配置 (Macros & Capacity Configurations)
 * ========================================================================== */

/** @brief 映射在 /dev/shm 下的 POSIX 共享内存文件名称 */
#define SHM_NAME "/kvs_xdp_shm_channel"

/** * @brief 环形队列的容量槽位数 
 * @note  必须严格保持为 2 的幂次方，以便利用位与运算 (&) 替代高昂的取模运算 (%) 
 */
#define SHM_RING_SIZE 524288 

/** @brief 环形队列的位掩码，用于指针越界绕回计算 */
#define SHM_RING_MASK (SHM_RING_SIZE - 1)

/** @brief 单个数据帧的最大有效载荷，严格适配标准以太网 MTU 尺寸 */
#define MAX_PAYLOAD 1400     

/* ==========================================================================
 * 核心数据结构与缓存行对齐 (Data Structures & Cache Line Alignment)
 * ========================================================================== */

/**
 * @brief 单个共享内存数据帧格式
 * @note  [核心微架构优化] 
 * uint32_t (4 字节) + data (1404 字节) = 1408 字节。
 * 1408 字节恰好为 64 的整数倍 (1408 / 64 = 22)。
 * 通过 __attribute__((aligned(64))) 确保结构体在环形数组中首尾完美衔接，
 * 绝对避免跨缓存行访问 (Cache Line Tearing) 导致的额外硬件周期开销。
 */
struct shm_pkt {
    uint32_t len;
    char data[1404]; 
} __attribute__((aligned(64))); 

/**
 * @brief 无锁 SPSC (Single-Producer Single-Consumer) 环形队列实体
 */
struct spsc_queue {
    /*
     * [防伪共享设计]
     * 通过 alignas(64) 强制隔离读写指针的内存布局布局。
     * 确保生产者的修改与消费者的修改落在完全不同的物理 Cache Line 上，
     * 阻止多核间的 MESI 缓存失效广播 (Invalidate) 风暴。
     */
    alignas(64) atomic_uint head; 
    alignas(64) atomic_uint tail; 
    
    struct shm_pkt ring[SHM_RING_SIZE];
};

/**
 * @brief 全双工 IPC 共享内存上下文
 */
struct shm_context {
    struct spsc_queue tx_queue; /**< 发送环形队列 */
    struct spsc_queue rx_queue; /**< 接收环形队列 */
};

/* ==========================================================================
 * 生命周期与无锁操作接口 (Lifecycle & Lock-free APIs)
 * ========================================================================== */

/**
 * @brief       初始化或挂载共享内存上下文
 * @param[in]   is_creator 标识当前是否为创建内存的主导进程
 * @return      struct shm_context* 成功返回内存映射基址；失败返回 NULL
 */
struct shm_context* shm_ipc_init(bool is_creator);

/**
 * @brief       将数据压入无锁 SPSC 环形队列
 * @param[in]   q     目标队列指针
 * @param[in]   data  待压入的数据基址
 * @param[in]   len   待压入的数据长度
 * @return      bool  true: 成功; false: 队列满或越界
 */
bool spsc_enqueue(struct spsc_queue *q, const char *data, uint32_t len);

/**
 * @brief       从无锁 SPSC 环形队列中弹出数据
 * @param[in]   q         目标队列指针
 * @param[out]  data_out  接收数据的预分配缓冲区
 * @param[out]  len_out   用于传出实际读取长度的指针
 * @return      bool      true: 成功; false: 队列空
 */
bool spsc_dequeue(struct spsc_queue *q, char *data_out, uint32_t *len_out);