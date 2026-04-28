/**
 * @file        shm_ipc.c
 * @brief       InazumaKV 极速共享内存进程间通信 (IPC) 模块
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        采用 POSIX 共享内存与无锁 (Lock-free) 环形缓冲区实现。
 * 针对 SPSC (Single-Producer Single-Consumer) 模型进行了极限优化：
 * 引入 Shadow Variables (影子指针) 与 C11 细粒度内存屏障 (Memory Order)，
 * 有效消除了 CPU 缓存行的伪共享 (False Sharing) 与不必要的总线嗅探开销。
 */

#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

/* ==========================================================================
 * 线程本地存储 (Thread Local Storage) 与 CPU 缓存优化
 * ========================================================================== */

/**
 * @brief 影子缓存指针 (Shadow Variables)
 * @note  使用 __thread 声明为线程局部变量。
 * 生产者缓存消费者的 head，消费者缓存生产者的 tail。
 * 此机制避免了高频的跨核 Cache Line 状态失效 (MESI 协议导致的 Invalidate)，
 * 仅在本地缓存判定容量不足时，才发起一次跨核的 acquire 语义同步。
 */
static __thread unsigned int local_cached_head = 0;
static __thread unsigned int local_cached_tail = 0;


/* ==========================================================================
 * 初始化与生命周期管理 (Lifecycle Management)
 * ========================================================================== */

/**
 * @brief       初始化或挂载共享内存上下文
 * @param[in]   is_creator 标识当前进程是否为创建者 (如 Master 进程)
 * @return      struct shm_context* 成功返回映射后的共享内存首地址；失败返回 NULL
 * @note        创建者负责申请并截断 (ftruncate) 物理内存，并初始化原子指针；
 * 挂载者采用退避轮询机制等待共享内存的就绪。
 */
struct shm_context* shm_ipc_init(bool is_creator) {
    int shm_fd;
    if (is_creator) {
        /* 防御性编程：清理历史残留的共享内存块 */
        shm_unlink(SHM_NAME);
        shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR | O_EXCL, 0666);
    } else {
        /* 挂载者采用自旋重试策略，应对并发启动时的时序竞争 */
        do {
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
            if (shm_fd < 0) usleep(100000);
        } while (shm_fd < 0);
    }

    if (shm_fd < 0) { 
        perror("shm_open failed"); 
        return NULL; 
    }

    size_t size = sizeof(struct shm_context);

    /* 仅创建者拥有截断分配物理内存段的权限 */
    if (is_creator) {
        if (ftruncate(shm_fd, size) == -1) { 
            perror("ftruncate failed"); 
            return NULL; 
        }
    }

    /* 内存映射：采用 MAP_SHARED 保障跨进程可见性 */
    void *ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) { 
        perror("mmap failed"); 
        return NULL; 
    }

    struct shm_context *ctx = (struct shm_context *)ptr;

    if (is_creator) {
        /* C11 标准原子变量初始化 */
        atomic_init(&ctx->tx_queue.head, 0);
        atomic_init(&ctx->tx_queue.tail, 0);
        atomic_init(&ctx->rx_queue.head, 0);
        atomic_init(&ctx->rx_queue.tail, 0);
        printf("[IPC] Shared memory created successfully. Size: %zu MB\n", size / 1024 / 1024);
    } else {
        printf("[IPC] Attached to shared memory successfully.\n");
    }

    /* fd 已通过 mmap 映射到虚拟地址空间，此处关闭 fd 不影响实际内存访问 */
    close(shm_fd); 
    return ctx;
}


/* ==========================================================================
 * 单生产者单消费者 (SPSC) 无锁环形队列核心逻辑
 * ========================================================================== */

/**
 * @brief       SPSC 无锁入队 (仅由生产者调用)
 * @param[in]   q     目标环形队列指针
 * @param[in]   data  待写入数据的连续内存首地址
 * @param[in]   len   待写入数据的有效字节长度
 * @return      bool  true: 入队成功; false: 队列已满或负载越界
 * @note        [核心机制] 利用影子头指针 (local_cached_head) 执行极速路径验证。
 * 数据写入完成后，使用 memory_order_release 发布尾指针，确保数据可见性。
 */
bool spsc_enqueue(struct spsc_queue *q, const char *data, uint32_t len) {
    if (len > MAX_PAYLOAD) return false;

    /* 获取自身维护的写入位置，本线程独占写入，使用 relaxed 语义即可 */
    unsigned int tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned int next_tail = (tail + 1) & SHM_RING_MASK;

    /* 极速路径验证：利用本地缓存的消费进度判定容量，避免触发主存总线嗅探 */
    if (next_tail == local_cached_head) {
        /* 缓存指示容量已满，发起跨核 acquire 读取最新消费进度以二次校验 */
        local_cached_head = atomic_load_explicit(&q->head, memory_order_acquire);
        if (next_tail == local_cached_head) {
            return false; /* 确认空间耗尽，向上层触发背压 (Backpressure) */
        }
    }

    /* 执行内存安全拷贝动作 */
    q->ring[tail].len = len;
    memcpy(q->ring[tail].data, data, len);

    /* * 发布数据机制 (Release Store)：
     * 保证 memcpy 指令不会被重排序到此 atomic_store 之后，通知消费者安全读取。
     */
    atomic_store_explicit(&q->tail, next_tail, memory_order_release);
    return true;
}

/**
 * @brief       SPSC 无锁出队 (仅由消费者调用)
 * @param[in]   q         目标环形队列指针
 * @param[out]  data_out  接收数据的预分配缓冲区指针
 * @param[out]  len_out   用于传出实际读取字节数的指针
 * @return      bool      true: 出队成功; false: 队列为空
 * @note        [核心机制] 利用影子尾指针 (local_cached_tail) 执行极速路径验证。
 * 读取数据后，使用 memory_order_release 发布头指针，通知生产者释放可用槽位。
 */
bool spsc_dequeue(struct spsc_queue *q, char *data_out, uint32_t *len_out) {
    /* 获取自身维护的读取位置，本线程独占读取，使用 relaxed 语义 */
    unsigned int head = atomic_load_explicit(&q->head, memory_order_relaxed);

    /* 极速路径验证：利用本地缓存的生产进度判定是否为空，实现零跨核争用 */
    if (head == local_cached_tail) {
        /* 缓存指示队列为空，发起跨核 acquire 获取生产者的最新提交进度 */
        local_cached_tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (head == local_cached_tail) {
            return false; /* 确认无可用数据 */
        }
    }

    /* 从共享内存环形数组中拷出负载数据 */
    *len_out = q->ring[head].len;
    memcpy(data_out, q->ring[head].data, *len_out);

    /*
     * 回收空间机制 (Release Store)：
     * 推进 head 指针并使用 release 语义，告知生产者当前槽位已安全空出可供覆写。
     */
    unsigned int next_head = (head + 1) & SHM_RING_MASK;
    atomic_store_explicit(&q->head, next_head, memory_order_release);
    return true;
}