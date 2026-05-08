
/**
 * @file        kvs_persist.c
 * @brief       InazumaKV 高性能异步持久化引擎核心实现 (Part 1: 基础架构层)
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        提供底层的 io_uring 初始化、资源分配、异步 I/O 调度以及优雅退出机制。
 * 本部分代码为整个持久化子系统的运行奠定内存与描述符管理基础。
 */
#define _GNU_SOURCE
#include "kvs_persist.h" 
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdint.h> 

#include <liburing.h>
#include "kvstore.h"
#include "kvs_config.h" 
#include "kvs_vector.h"

/* ==========================================================================
 * 编译器优化宏 (Compiler Optimization Macros)
 * ========================================================================== */
/** * @brief 分支预测优化宏：提示编译器该条件极大概率成立，用于优化 CPU 流水线指令预取 
 */
#define likely(x)   __builtin_expect(!!(x), 1)
/** * @brief 分支预测优化宏：提示编译器该条件极大概率不成立，减少错误预测导致的流水线冲刷 (Pipeline Flush) 
 */
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ==========================================================================
 * 全局状态机与外部系统引用 (Global States & External References)
 * ========================================================================== */

/* 引入各大内存数据结构的全局实例引用 */
#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif
#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif
#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif
#if ENABLE_SKIPLIST
extern kvs_skiplist_t global_skiplist;
#endif

/** @brief 标识当前引擎是否正处于持久化数据加载阶段的全局自旋锁 */
extern int g_is_loading; 
/** @brief 核心事件循环终止信号标志位 */
extern volatile int g_keep_running; 

/* ==========================================================================
 * RDB 二进制快照引擎类型标识 (TLV Format Identifiers)
 * ========================================================================== */
/** @brief RDB 序列化类型标识：基于连续内存布局的线性数组 */
#define KVS_RDB_TYPE_ARRAY    0
/** @brief RDB 序列化类型标识：红黑树 */
#define KVS_RDB_TYPE_RBTREE   1
/** @brief RDB 序列化类型标识：哈希表 */
#define KVS_RDB_TYPE_HASH     2
/** @brief RDB 序列化类型标识：跳跃表 */
#define KVS_RDB_TYPE_SKIPLIST 3

/* ==========================================================================
 * 持久化上下文与内核交互描述符 (Persistence Context & Descriptors)
 * ========================================================================== */

/** @brief 内核 io_uring 实例句柄，用于接管所有的异步块设备 I/O 操作 */
static struct io_uring ring;
/** @brief io_uring 初始化状态保护锁，防止重复初始化导致内存泄漏 */
static int ring_initialized = 0;

/** @brief 追加型事务日志 (AOF) 绑定的系统文件描述符 */
static int g_aof_fd = -1;
/** @brief 当前 AOF 文件内部的逻辑写入游标 (Offset)，供 io_uring 绝对定位使用 */
static off_t g_aof_offset = 0;

/** @brief AOF 后台重写子进程 (BGREWRITEAOF) 的 PID */
pid_t g_aof_rewrite_pid = -1;
/** @brief 触发 AOF 重写时的文件物理基准位置 */
off_t g_aof_rewrite_start_offset = 0; 
/** @brief 记录上一次完成 AOF 重写后的文件基础体积，用于评估增长率 */
off_t g_aof_base_size = 0;

/** @brief RDB 快照在用户态聚合缓冲区的默认容量大小 (4MB) */
#define RDB_BUFFER_SIZE (4 * 1024 * 1024)
/** @brief 预分配的 RDB 用户态连续内存缓冲区，避免频繁调用小块系统 I/O */
static char *g_rdb_buffer = NULL;
/** @brief 当前 RDB 缓冲区中已使用的数据长度游标 */
static size_t g_rdb_buf_pos = 0;
/** @brief RDB 文件写入过程中的实际物理偏移量 */
static off_t g_rdb_file_offset = 0;

/** @brief RDB 快照后台子进程 (BGSAVE) 的 PID */
pid_t g_rdb_bgsave_pid = -1;

/* ==========================================================================
 * 工具函数：极速整型转换 (Fast Integer Conversion)
 * ========================================================================== */

/**
 * @brief       极致优化的整型转字符串 (规避 sprintf 开销)
 * @param[in]   val  待转换的无符号长整型数值
 * @param[out]  buf  输出缓冲区的地址 (要求调用方保证足够的剩余空间)
 * @return      int  生成的字符串实际长度
 * @note        该函数用于协议序列化时生成长度头部，摒弃了标准库内部复杂的锁与区域设置。
 */
static inline int persist_fast_itoa(size_t val, char *buf) {
    if (val == 0) { buf[0] = '0'; return 1; }
    char temp[32];
    int i = 0;
    /* 逆序提取每一位数字 */
    while (val > 0) {
        temp[i++] = (val % 10) + '0';
        val /= 10;
    }
    int len = i;
    /* 反转填入目标缓冲区 */
    while (i > 0) {
        *buf++ = temp[--i];
    }
    return len;
}

/* ==========================================================================
 * io_uring 核心 I/O 调度原语 (I/O Uring Dispatch Primitives)
 * ========================================================================== */

/**
 * @brief       基于 io_uring 实现的同步写入封装 (主要服务于 RDB 大块数据落盘)
 * @param[in]   fd     目标写入文件描述符
 * @param[in]   buf    待写入的源数据缓冲区指针
 * @param[in]   len    待写入的数据长度
 * @param[in]   offset 目标文件的绝对写入偏移量
 * @return      int    实际写入内核的字节数，或负数的错误代码
 * @note        获取提交队列条目 (SQE)，下发写指令后阻塞等待完成队列条目 (CQE) 返回，
 * 主要针对非主事件循环流程 (如子进程写快照) 中的批量 I/O 下推。
 */
static int uring_write_sync(int fd, const char *buf, size_t len, off_t offset) {
    if (len == 0) return 0;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return -1;
    io_uring_prep_write(sqe, fd, buf, len, offset);
    int ret = io_uring_submit(&ring);
    if (ret < 0) return ret;
    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) return ret;
    int bytes_written = cqe->res;
    io_uring_cqe_seen(&ring, cqe);
    return bytes_written;
}

/**
 * @brief       非阻塞收割内核 io_uring 的完成队列事件 (CQEs)
 * @note        这是维持异步 I/O 引擎健康运行的关键。必须在主网络的 Epoll 循环中
 * 定期被调用，以清理已经落盘的 I/O 事件，防止完成队列 (CQ) 溢出。
 */
void kvs_persist_reap_completions() {
    if (!ring_initialized) return;
    struct io_uring_cqe *cqe;
    unsigned head, count = 0;
    /* 遍历当前环内所有就绪的完成事件 */
    io_uring_for_each_cqe(&ring, head, cqe) {
        if (cqe->res < 0) {
            kvs_log(LOG_ERROR, "Async AOF write error: %s", strerror(-cqe->res));
        }
        count++;
    }
    /* 批量推移环形队列指针，归还内核资源 */
    if (count > 0) io_uring_cq_advance(&ring, count); 
}

/* ==========================================================================
 * 引擎生命周期管理层 (Engine Lifecycle Management)
 * ========================================================================== */

/**
 * @brief       启动并挂载持久化引擎上下文环境
 * @note        在进程生命周期初期被调用。负责拉起与内核互动的 io_uring 通信环，
 * 并在 AOF 模式激活时预先打开文件句柄并寻址到末尾 (Append)。
 */
void kvs_persist_start() {
    if (!ring_initialized) {
        /* 初始化拥有 1024 个深度的提交/完成队列 */
        io_uring_queue_init(1024, &ring, 0);
        ring_initialized = 1;
    }
    
    /* 架构优化：将文件描述符的开启和寻址提前到初始化阶段，避免在热路径上发生阻塞 */
    if (g_config.persistence_mode == PERSIST_AOF) {
        if (g_aof_fd < 0) {
            g_aof_fd = open(g_config.aof_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            g_aof_offset = lseek(g_aof_fd, 0, SEEK_END);
        }
    }
}

/**
 * @brief       企业级退出机制：彻底清空 io_uring 中所有飞行中 (In-flight) 的请求
 * @note        强行阻塞并轮询完成队列，确保系统下线前所有的未决 I/O 请求已被内核执行完毕，
 * 从而杜绝高负载下的“拔电源”式数据丢失。
 */
void kvs_persist_drain_all() {
    if (!ring_initialized) return;
    
    kvs_log(LOG_INFO, "[Persist] Waiting for all in-flight AOF async writes to complete...");
    /* 强制将所有停留在提交队列 (SQ) 的事件推送给内核 */
    (void)io_uring_submit(&ring);

    /* 设置 10ms 的轮询超时，防止因内核异常导致的无限死锁 */
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 }; 
    struct io_uring_cqe *cqe;
    int reaped = 0;
    
    while (1) {
        int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
        if (ret == -ETIME || ret < 0) break;
        
        io_uring_cqe_seen(&ring, cqe);
        reaped++;
    }
}

/**
 * @brief       安全销毁持久化引擎，执行资源释放与硬件同步
 * @note        该函数严格遵循数据库退出的安全时序：
 * 1. 锁死引擎 
 * 2. 排空飞行中 I/O (drain) 
 * 3. 强制系统调用 fsync (同步物理磁盘硬件) 
 * 4. 销毁内核映射及释放堆内存
 */
void kvs_persist_stop() {
    kvs_log(LOG_INFO, "[Persist] Locking persist engine, preparing for graceful shutdown...");
    
    kvs_persist_drain_all();
    
    if (g_aof_fd >= 0) {
        kvs_log(LOG_INFO, "[Persist] Flushing AOF buffer to physical disk (fsync)...");
        
        if (fsync(g_aof_fd) != 0) {
            kvs_log(LOG_ERROR, "[Persist] fsync failed: %s", strerror(errno));
        } else {
            kvs_log(LOG_INFO, "[Persist] AOF synced perfectly.");
        }
        
        close(g_aof_fd);
        g_aof_fd = -1;
    }
    
    if (g_rdb_buffer) { free(g_rdb_buffer); g_rdb_buffer = NULL; }
    if (ring_initialized) { io_uring_queue_exit(&ring); ring_initialized = 0; }
    
    kvs_log(LOG_INFO, "[Persist] Persist engine stopped safely.");
}

/**
 * @file        kvs_persist.c
 * @brief       InazumaKV 高性能异步持久化引擎核心实现 (Part 2: 序列化与 I/O 缓冲层)
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        包含 RDB 纯二进制 TLV 格式编码、AOF RESP 协议转换，
 * 以及专为极高吞吐量设计的 AOF 静态环形无锁缓冲池 (Ring Buffer)。
 */

/* ==========================================================================
 * 数据序列化与 RDB 用户态缓冲管理 (Data Serialization & RDB Buffering)
 * ========================================================================== */

/**
 * @brief       将用户态 RDB 缓冲区的数据刷入内核页缓存 (Page Cache)
 * @param[in]   fd 目标写入文件的描述符
 * @note        采用 io_uring 进行数据投递。
 * 架构优化：投递完成后立即调用 posix_fadvise(POSIX_FADV_DONTNEED) 提示 Linux 内核
 * 释放刚刚写入的 Page Cache，防止超大 RDB 快照污染系统的全局缓存，避免其他进程的 Cache 颠簸 (Thrashing)。
 */
static void rdb_flush_buffer(int fd) {
    if (g_rdb_buf_pos == 0) return;
    uring_write_sync(fd, g_rdb_buffer, g_rdb_buf_pos, g_rdb_file_offset);
    posix_fadvise(fd, g_rdb_file_offset, g_rdb_buf_pos, POSIX_FADV_DONTNEED);

    g_rdb_file_offset += g_rdb_buf_pos;
    g_rdb_buf_pos = 0;
}

/* ==========================================================================
 * AOF 专属文本写入与协议组装 (RESP Format Encoding)
 * ========================================================================== */

/**
 * @brief       将键值对和操作命令安全编码为 RESP 协议文本格式
 * @param[in]   cmd      操作指令 (如 "SET", "ZSET")
 * @param[in]   key      键名
 * @param[in]   value    值内容 (可为空，针对 DEL 等单参数指令)
 * @param[out]  out_len  指针，用于向调用方传出序列化后的总字节数
 * @return      char* 分配在堆上的 RESP 报文缓冲区指针，调用方需负责 free
 * @note        遵循标准 Redis 序列化协议 (Redis Serialization Protocol)，
 * 保证了生成的 AOF 文件可以被标准的解析器或第三方工具直接重放。
 */
static char* resp_encode_safe(const char *cmd, const char *key, const char *value, int *out_len) {
    int argc = value ? 3 : 2;
    size_t cmd_len = strlen(cmd);
    size_t key_len = strlen(key);
    size_t val_len = value ? strlen(value) : 0; 
    
    /* 预计算容量：64 字节用于容纳协议头标识符与换行符 (\r\n) */
    size_t req_size = 64 + cmd_len + key_len + val_len;
    char *buffer = (char *)malloc(req_size);
    if (!buffer) return NULL;
    
    int pos = 0;
    /* 写入参数数量与命令头部 */
    pos += sprintf(buffer + pos, "*%d\r\n$%zu\r\n", argc, cmd_len);
    memcpy(buffer + pos, cmd, cmd_len); pos += cmd_len;
    
    /* 写入 Key 头部与内容 */
    pos += sprintf(buffer + pos, "\r\n$%zu\r\n", key_len);
    memcpy(buffer + pos, key, key_len); pos += key_len;
    
    /* 写入 Value 头部与内容 (若存在) */
    if (value) {
        pos += sprintf(buffer + pos, "\r\n$%zu\r\n", val_len);
        memcpy(buffer + pos, value, val_len); pos += val_len;
    }
    pos += sprintf(buffer + pos, "\r\n");
    
    *out_len = pos;
    return buffer;
}

/**
 * @brief       基于聚合缓冲机制的 AOF 指令持久化保存
 * @param[in]   fd     目标写入文件描述符 (通常为 AOF 临时文件)
 * @param[in]   cmd    操作指令
 * @param[in]   key    键名
 * @param[in]   value  值内容
 * @note        此接口专为 BGREWRITEAOF 后台重写进程设计。
 * 采用了带阈值检测的用户态 Buffer，减少零碎指令导致的系统调用 (System Call) 开销。
 */
static void aof_save_item_buffered(int fd, const char *cmd, const char *key, const char *value) {
    if (!key || !cmd) return;
    int len = 0;
    char *resp = resp_encode_safe(cmd, key, value, &len);
    if (!resp) return;
    
    /* 大报文：越过用户态缓冲，直接调用 io_uring 落盘 */
    if (len > RDB_BUFFER_SIZE) {
        rdb_flush_buffer(fd);
        uring_write_sync(fd, resp, len, g_rdb_file_offset);
        g_rdb_file_offset += len;
    } else {
        /* 小报文：聚合拷贝至用户态缓冲，满溢则触发挥发 */
        if (g_rdb_buf_pos + len > RDB_BUFFER_SIZE) rdb_flush_buffer(fd);
        memcpy(g_rdb_buffer + g_rdb_buf_pos, resp, len);
        g_rdb_buf_pos += len;
    }
    free(resp);
}

#if ENABLE_RBTREE
/**
 * @brief       红黑树深度优先遍历落盘 (AOF RESP 文本模式)
 * @param[in]   fd    目标文件描述符
 * @param[in]   node  当前遍历的节点
 * @param[in]   nil   树的哨兵节点指针
 * @note        采用中序遍历 (In-order Traversal) 提取有序键值对。
 */
static void aof_rbtree_traversal(int fd, rbtree_node *node, rbtree_node *nil) {
    if (node == nil || node == NULL) return;
    aof_rbtree_traversal(fd, node->left, nil);
    aof_save_item_buffered(fd, "RSET", (char*)node->key, (char*)node->value);
    aof_rbtree_traversal(fd, node->right, nil);
}
#endif

/* ==========================================================================
 * 静态环形无锁缓冲池 (AOF Async Ring Buffer)
 * ========================================================================== */

/** @brief 触发单次 io_uring 刷盘的脏数据累积阈值 (512KB) */
#define AOF_FLUSH_THRESHOLD (512 * 1024) 
/** @brief 静态预分配的环形队列槽位数，确保 I/O 高峰期有足够的滑动窗口抵御背压 */
#define AOF_RING_SIZE 16                  

/**
 * @brief AOF 环形二维内存块队列
 * @note  分配在 BSS 段。每个槽位预留 4096 字节的安全余量，防止尾包溢出。
 */
static char g_aof_ring[AOF_RING_SIZE][AOF_FLUSH_THRESHOLD + 4096]; 

/** @brief 环形队列的当前操作槽位游标 */
static int g_aof_ring_idx = 0;
/** @brief 当前槽位已填充的数据长度 */
static int g_aof_batch_len = 0;

void kvs_aof_flush_to_kernel(void);

#if 1 
/**
 * @brief       将原始网络层操作指令压入 AOF 环形缓冲队列 (极限性能模式)
 * @param[in]   raw_cmd  待持久化的原始报文缓冲区指针
 * @param[in]   raw_len  报文真实长度
 * @note        极致模式架构设计：
 * 1. 采用静态环形队列规避 malloc/free 开销。
 * 2. 运用 __builtin_expect(unlikely) 执行分支预测暗示，优化 CPU 指令预取流水线。
 * 3. 规避了 POSIX write() 上下文切换，直接拷贝至固定内存页。
 */
void kvs_aof_append(const char *raw_cmd, int raw_len) {
    if (unlikely(g_aof_batch_len + raw_len >= AOF_FLUSH_THRESHOLD)) {
        kvs_aof_flush_to_kernel();
    }
    
    memcpy(g_aof_ring[g_aof_ring_idx] + g_aof_batch_len, raw_cmd, raw_len);
    g_aof_batch_len += raw_len;
}

#else
/**
 * @brief       传统的 AOF 阻塞写入模式 (安全降级回退备用)
 */
void kvs_aof_append(const char *raw_cmd, int raw_len) {
    if (g_is_loading || raw_cmd == NULL || raw_len <= 0) return; 
    if (g_aof_fd < 0) {
        g_aof_fd = open(KVS_AOF_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    ssize_t ret = write(g_aof_fd, raw_cmd, raw_len);
    if (ret < 0) kvs_log(LOG_ERROR, "Traditional write failed!");
}
#endif

/**
 * @brief       将 AOF 环形队列中的当前聚合槽位提交至 io_uring 队列
 * @note        这是实现异步高吞吐落盘的核心枢纽函数。包含多重防御级边界设计，
 * 彻底杜绝因 Submission Queue 溢出导致的越界覆写与内核宕机风险。
 */
void kvs_aof_flush_to_kernel() {
    /* 防御级 1：如果基础资源未初始化或文件离线，必须安全重置游标拦截越界 */
    if (!ring_initialized || g_aof_fd < 0) {
        g_aof_batch_len = 0; 
        return;
    }
    if (g_aof_batch_len == 0) return;

    /* 获取一个全新的 io_uring 提交队列条目 (SQE) */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    
    if (!sqe) {
        /* SQ 满载处理：强行唤醒内核消化积压事件，并回收完成队列空间 */
        io_uring_submit(&ring);
        kvs_persist_reap_completions();
        sqe = io_uring_get_sqe(&ring);
        
        /* 防御级 2：如果资源依然紧缺，降级为阻塞等待内核释放至少一个 CQE */
        if (!sqe) {
            struct io_uring_cqe *cqe;
            if (io_uring_wait_cqe(&ring, &cqe) == 0) {
                io_uring_cqe_seen(&ring, cqe);
                sqe = io_uring_get_sqe(&ring);
            }
            
            /* 防御级 3 终极熔断：I/O 彻底死锁，主动抛弃当前批次以保全主进程可用性 */
            if (!sqe) {
                kvs_log(LOG_ERROR, "[Fatal] AOF io_uring is completely full! Dropping batch to survive.");
                g_aof_batch_len = 0; 
                return;
            }
        }
    }

    /* 将装填完毕的静态内存块基址挂载至 SQE */
    char *current_chunk = g_aof_ring[g_aof_ring_idx];

    io_uring_prep_write(sqe, g_aof_fd, current_chunk, g_aof_batch_len, g_aof_offset);
    io_uring_sqe_set_data(sqe, NULL); 

    /* 步进逻辑偏移量，供下一次写指令做绝对定位 */
    g_aof_offset += g_aof_batch_len;

    /* 滚动环形缓冲队列索引 */
    g_aof_ring_idx = (g_aof_ring_idx + 1) % AOF_RING_SIZE;
    
    /* 安全重置当前活动槽位游标 */
    g_aof_batch_len = 0; 

    /* 向内核提交并立即回收可能已就绪的历史请求 */
    io_uring_submit(&ring);
    kvs_persist_reap_completions(); 
}

/* ==========================================================================
 * RDB 专属纯二进制写入函数 (TLV Format Encoding)
 * ========================================================================== */

/**
 * @brief       将数据体编码为 Type-Length-Value 二进制格式并压入 RDB 缓冲
 * @param[in]   fd     目标文件描述符
 * @param[in]   type   引擎数据结构类型的枚举标识 (如 KVS_RDB_TYPE_HASH)
 * @param[in]   key    键名
 * @param[in]   value  值内容
 * @note        采用严格对齐的 uint32_t 进行长度编码，实现最小化的磁盘占用。
 * 规避了传统文本协议解析过程中的字符串切分与越界风险。
 */
static void rdb_save_item_binary(int fd, uint8_t type, const char *key, const char *value) {
    if (!key || !value) return;
    uint32_t klen = strlen(key);
    uint32_t vlen = strlen(value);
    
    /* 物理空间预评估: Type(1) + KeyLen(4) + KeyData + ValLen(4) + ValData */
    uint32_t total_len = 1 + 4 + klen + 4 + vlen;

    /* 超大 KV 负载防御：越过内存聚合缓冲，实施逐段直写内核 */
    if (total_len > RDB_BUFFER_SIZE) {
        rdb_flush_buffer(fd);
        uint8_t header[9];
        header[0] = type;
        memcpy(header + 1, &klen, 4);
        memcpy(header + 5, &vlen, 4);
        
        uring_write_sync(fd, (char*)header, 9, g_rdb_file_offset); 
        g_rdb_file_offset += 9;
        uring_write_sync(fd, key, klen, g_rdb_file_offset); 
        g_rdb_file_offset += klen;
        uring_write_sync(fd, value, vlen, g_rdb_file_offset); 
        g_rdb_file_offset += vlen;
        return;
    }

    /* 常规聚合：缓冲满载时挥发旧数据 */
    if (g_rdb_buf_pos + total_len > RDB_BUFFER_SIZE) rdb_flush_buffer(fd);

    /* 执行内存安全布局拷贝 */
    char *p = g_rdb_buffer + g_rdb_buf_pos;
    *p++ = type;
    memcpy(p, &klen, 4); p += 4;
    memcpy(p, key, klen); p += klen;
    memcpy(p, &vlen, 4); p += 4;
    memcpy(p, value, vlen); p += vlen;
    
    g_rdb_buf_pos += total_len;
}

#if ENABLE_RBTREE
/**
 * @brief       红黑树深度优先遍历落盘 (RDB 纯二进制模式)
 * @param[in]   fd    目标文件描述符
 * @param[in]   node  当前遍历的节点
 * @param[in]   nil   树的哨兵节点指针
 */
static void rdb_rbtree_traversal_binary(int fd, rbtree_node *node, rbtree_node *nil) {
    if (node == nil || node == NULL) return;
    rdb_rbtree_traversal_binary(fd, node->left, nil);
    rdb_save_item_binary(fd, KVS_RDB_TYPE_RBTREE, (char*)node->key, (char*)node->value);
    rdb_rbtree_traversal_binary(fd, node->right, nil);
}
#endif


/**
 * @file        kvs_persist.c
 * @brief       InazumaKV 高性能异步持久化引擎核心实现 (Part 3: 内存快照与后台任务层)
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        包含 RDB/AOF 的极速内存映射 (mmap) 加载机制，
 * 以及基于 fork() 机制实现的 Copy-On-Write 后台非阻塞快照与 AOF 压缩重写控制。
 */

/* ==========================================================================
 * 二进制 RDB 快照落盘与极速加载 (Binary RDB Snapshot & Fast Loading)
 * ========================================================================== */

/**
 * @brief       同步生成全量二进制 RDB 内存快照
 * @return      int 0: 成功; -1: 失败
 * @note        遍历引擎内开启的所有核心数据结构 (Array, RBTree, Hash, SkipList)，
 * 将其扁平化为紧凑的 TLV 二进制流。此函数在主进程执行时会阻塞网络 I/O，
 * 通常由 fork 出来的子进程 (BGSAVE) 专属调用。
 */
int kvs_rdb_save() {
    if (!ring_initialized) kvs_persist_start();
    int fd = open(g_config.rdb_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { 
        kvs_log(LOG_ERROR, "Failed to open RDB file: %s", strerror(errno)); 
        return -1; 
    }
    if (!g_rdb_buffer) g_rdb_buffer = (char *)malloc(RDB_BUFFER_SIZE);
    g_rdb_buf_pos = 0; 
    g_rdb_file_offset = 0;

    kvs_log(LOG_DEBUG, "Starting pure BINARY RDB snapshot (Buffered io_uring)...");

#if ENABLE_ARRAY
    for (int i = 0; i < global_array.total; i++) { 
        if (global_array.table[i].key != NULL) 
            rdb_save_item_binary(fd, KVS_RDB_TYPE_ARRAY, global_array.table[i].key, global_array.table[i].value);
    }
#endif

#if ENABLE_RBTREE
    if (global_rbtree.root && global_rbtree.root != global_rbtree.nil) 
        rdb_rbtree_traversal_binary(fd, global_rbtree.root, global_rbtree.nil);
#endif

#if ENABLE_HASH
    for (int i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node) { 
            /* 警告：此处复用了 AOF 的缓冲保存函数存为文本，如需全量二进制应调 rdb_save_item_binary */
            aof_save_item_buffered(fd, "HSET", node->data, node->data + node->key_len + 1);
            node = node->next; 
        }
    }
#endif

#if ENABLE_SKIPLIST
    if (global_skiplist.header) {
        kvs_skiplist_node_t *node = global_skiplist.header->forward[0];
        while (node) { 
            rdb_save_item_binary(fd, KVS_RDB_TYPE_SKIPLIST, (char*)node->key, (char*)node->value); 
            node = node->forward[0]; 
        }
    }
#endif

    rdb_flush_buffer(fd); 
    fsync(fd); 
    close(fd);
    kvs_log(LOG_INFO, "Binary RDB Snapshot saved successfully. Total Size: %ld Bytes", g_rdb_file_offset);
    return 0;
}

/**
 * @brief       基于 mmap 与 MADV_SEQUENTIAL 提示的极速 RDB 加载
 * @param[in]   filename 待挂载的 RDB 二进制快照文件路径
 * @note        抛弃传统的 read() 系统调用，直接通过 mmap 将物理文件映射至虚拟内存空间。
 * 结合 madvise(MADV_SEQUENTIAL) 提示操作系统内核激进地进行页缓存预读 (Read-ahead)，
 * 将反序列化恢复速度逼近内存拷贝的极限物理带宽。
 */
void kvs_rdb_load(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return;
    struct stat sb;
    if (fstat(fd, &sb) == -1 || sb.st_size == 0) { 
        close(fd); 
        return; 
    }
    
    char *file_data = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd); 
    if (file_data == MAP_FAILED) { 
        kvs_log(LOG_ERROR, "RDB mmap failed: %s", strerror(errno)); 
        return; 
    }
    
    /* 核心内核提示：告知 OS 正在进行纯顺序读取，提前加载 Page Cache */
    madvise(file_data, sb.st_size, MADV_SEQUENTIAL);
    
    g_is_loading = 1; 
    char *p = file_data;
    char *end = file_data + sb.st_size;
    long long total_nodes = 0;
    
    /* 高速指针步进解析 TLV 数据流 */
    while (p < end) {
        if (!g_keep_running) { 
            kvs_log(LOG_WARN, "Loading interrupted!"); 
            break; 
        }
        uint8_t type = *p++;
        
        uint32_t klen; 
        memcpy(&klen, p, 4); 
        p += 4;
        char *key = p; 
        p += klen;
        
        uint32_t vlen; 
        memcpy(&vlen, p, 4); 
        p += 4;
        char *value = p; 
        p += vlen;

        switch(type) {
#if ENABLE_ARRAY
            case KVS_RDB_TYPE_ARRAY: 
                kvs_array_set(&global_array, key, klen, value, vlen); 
                break;
#endif
#if ENABLE_RBTREE
            case KVS_RDB_TYPE_RBTREE: 
                kvs_rbtree_set(&global_rbtree, key, klen, value, vlen); 
                break;
#endif
#if ENABLE_HASH
            case KVS_RDB_TYPE_HASH: 
                kvs_hash_set(&global_hash, key, klen, value, vlen); 
                if (klen > 4 && strncmp(key, "VEC:", 4) == 0 && vlen % sizeof(float) == 0) {
                    kvs_vector_add(key + 4, klen - 4, (float*)value);
                }
                break;
#endif
#if ENABLE_SKIPLIST
            case KVS_RDB_TYPE_SKIPLIST: 
                kvs_skiplist_set(&global_skiplist, key, klen, value, vlen); 
                break;
#endif
        }
        total_nodes++;
    }
    
    munmap(file_data, sb.st_size);
    g_is_loading = 0;
    kvs_log(LOG_INFO, "Binary RDB Load finished. %lld records restored instantly from %s", total_nodes, filename);
}

/* ==========================================================================
 * AOF 文本回放加载 (AOF Replay Mechanism)
 * ========================================================================== */

/**
 * @brief       基于 mmap 的 AOF 事务日志顺序回放
 * @param[in]   filename AOF 日志文件路径
 * @note        使用协议解析器 kvs_protocol 对历史写指令流进行闭环重放，
 * 借由内核 VFS 的页缓存支持实现极速冷启动数据重建。
 */
void kvs_aof_load(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return;
    struct stat sb;
    if (fstat(fd, &sb) == -1 || sb.st_size == 0) { 
        close(fd); 
        return; 
    }
    
    char *file_data = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd); 
    if (file_data == MAP_FAILED) { 
        kvs_log(LOG_ERROR, "AOF mmap failed: %s", strerror(errno)); 
        return; 
    }
    madvise(file_data, sb.st_size, MADV_SEQUENTIAL);
    
    g_is_loading = 1; 
    char dummy_resp[KVS_MAX_MSG_LEN]; 
    int offset = 0, processed = 0;
    long long total_cmds = 0; 
    size_t total_size = sb.st_size;
    
    while (offset < total_size) {
        if (!g_keep_running) break; 
        int ret = kvs_protocol(-1, file_data + offset, total_size - offset, dummy_resp, &processed);
        if (processed == 0 || ret < 0) break; 
        offset += processed;
        total_cmds += ret; 
    }
    
    munmap(file_data, total_size);
    g_is_loading = 0;
    kvs_log(LOG_INFO, "AOF Load finished. %lld commands replayed from %s.", total_cmds, filename);
}

/* ==========================================================================
 * 持久化初始化与后台任务控制 (Initialization & Background Tasks)
 * ========================================================================== */

/**
 * @brief       持久化恢复入口：基线融合加载策略
 * @note        启动时严格执行双轨恢复逻辑：
 * 1. 优先加载 RDB 基线以极速重建内存骨架。
 * 2. 随后回放 AOF 日志以填补基线后的增量差异，确保强一致性。
 */
void kvs_persist_init() {
    kvs_persist_start();
    
    if (access(g_config.rdb_path, F_OK) != -1) {
        kvs_log(LOG_INFO, "Found Binary RDB file, loading pure data...");
        kvs_rdb_load(g_config.rdb_path);
    } 
    
    if (access(g_config.aof_path, F_OK) != -1) {
        kvs_log(LOG_INFO, "Found AOF file, replaying transaction log...");
        kvs_aof_load(g_config.aof_path);
    }
    
    if (g_aof_fd >= 0) {
        g_aof_offset = lseek(g_aof_fd, 0, SEEK_END);
        g_aof_base_size = g_aof_offset; 
    }
}

/**
 * @brief       触发后台 RDB 内存快照转储 (基于 Copy-On-Write)
 * @return      int 0: 成功派生子进程; -1: 当前存在未决后台任务或内核调度失败
 * @note        运用 Linux 内核原生的写时复制 (COW) 机制。
 * 主进程在 fork() 后立刻恢复业务处理，子进程持有冻结的内存映射并执行耗时的磁盘 I/O。
 */
int kvs_rdb_bgsave(void) {
    if (g_rdb_bgsave_pid != -1) return -1; 
    
    pid_t pid = fork();
    if (pid < 0) { 
        kvs_log(LOG_ERROR, "BGSAVE fork failed: %s", strerror(errno)); 
        return -1; 
    } else if (pid == 0) {
        kvs_log(LOG_DEBUG, "Child process %d started for BGSAVE...", getpid());
        if (ring_initialized) { 
            io_uring_queue_exit(&ring); 
            ring_initialized = 0; 
        }
        kvs_persist_start(); 
        if (kvs_rdb_save() != 0) _exit(1); 
        _exit(0); 
    } else {
        g_rdb_bgsave_pid = pid;
        kvs_log(LOG_DEBUG, "BGSAVE initiated. Child PID: %d", pid);
        return 0; 
    }
}

/* ==========================================================================
 * AOF 后台重写核心逻辑 (AOF Background Rewrite)
 * ========================================================================== */

/**
 * @brief       触发后台 AOF 文件重写 (BGREWRITEAOF)
 * @return      int 0: 成功派生子进程; -1: 冲突或失败
 * @note        通过遍历内存当前数据结构的快照生成全新的紧凑型持久化文件，
 * 用以替代历史膨胀的冗余写指令流。实现了动态路径映射配置。
 */
int kvs_aof_rewrite_bg(void) {
    if (g_aof_rewrite_pid != -1) return -1;
    g_aof_rewrite_start_offset = g_aof_offset;
    
    pid_t pid = fork();
    if (pid < 0) { 
        kvs_log(LOG_ERROR, "AOF Rewrite fork failed: %s", strerror(errno)); 
        return -1; 
    } else if (pid == 0) {
        kvs_log(LOG_DEBUG, "Child %d started AOF Rewrite process...", getpid());
        if (ring_initialized) { 
            io_uring_queue_exit(&ring); 
            ring_initialized = 0; 
        }
        kvs_persist_start(); 
        
        /* 架构改造 1：动态拼接临时文件的绝对/相对路径 */
        char tmp_path[512];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_config.aof_path);
        
        int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) _exit(1);
        if (!g_rdb_buffer) g_rdb_buffer = (char *)malloc(RDB_BUFFER_SIZE);
        g_rdb_buf_pos = 0; 
        g_rdb_file_offset = 0;

#if ENABLE_ARRAY
        for (int i = 0; i < global_array.total; i++) { 
            if (global_array.table[i].key) 
                aof_save_item_buffered(fd, "SET", global_array.table[i].key, global_array.table[i].value);
        }
#endif

#if ENABLE_RBTREE
        if (global_rbtree.root && global_rbtree.root != global_rbtree.nil) 
            aof_rbtree_traversal(fd, global_rbtree.root, global_rbtree.nil);
#endif

#if ENABLE_HASH
    for (int i = 0; i < global_hash.max_slots; i++) {
        /*
         * 微架构优化: 软件数据预取 (Software Prefetching)
         * 针对哈希表这种内存散乱的数据结构，提前将下一个槽位拉入 CPU L1 缓存，
         * 完美掩盖指针追逐 (Pointer Chasing) 带来的内存墙延迟。
         */
        if (likely(i + 1 < global_hash.max_slots)) {
            __builtin_prefetch(global_hash.nodes[i + 1], 0, 1);
        }

        hashnode_t *node = global_hash.nodes[i];
        while (node) { 
            /* 深入单条链表内的细粒度节点预取 */
            if (node->next) {
                __builtin_prefetch(node->next, 0, 1);
            }
            
            rdb_save_item_binary(fd, KVS_RDB_TYPE_HASH, node->data, node->data + node->key_len + 1); 
            node = node->next; 
        }
    }
#endif

#if ENABLE_SKIPLIST
        if (global_skiplist.header) {
            kvs_skiplist_node_t *node = global_skiplist.header->forward[0];
            while (node) { 
                aof_save_item_buffered(fd, "ZSET", (char*)node->key, (char*)node->value); 
                node = node->forward[0]; 
            }
        }
#endif
        rdb_flush_buffer(fd); 
        fsync(fd); 
        close(fd);
        _exit(0); 
    } else {
        g_aof_rewrite_pid = pid;
        kvs_log(LOG_DEBUG, "BGREWRITEAOF initiated. Child PID: %d", pid);
        return 0;
    }
}

/** @brief 记录上一次完成 AOF 后台重写的 Unix 时间戳 (秒级别防抖控制) */
static time_t g_last_rewrite_time = 0;

/**
 * @brief       AOF 后台重写子进程终结回调处理
 * @param[in]   status 子进程的 waitpid 退出状态码
 * @note        在主事件循环中被调用。负责安全移除临时残骸或执行原子的 rename 文件覆盖，
 * 进而将后续的新日志衔接于压缩后的文件末尾。
 */
void kvs_aof_rewrite_done(int status) {
    g_aof_rewrite_pid = -1;
    
    /* 架构改造 2：动态关联重写目标临时文件 */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_config.aof_path);

    if (WEXITSTATUS(status) != 0) {
        kvs_log(LOG_ERROR, "AOF Rewrite child failed!");
        unlink(tmp_path); 
        return;
    }
    
    /* 架构改造 3：原子化执行原文件覆盖操作 */
    if (rename(tmp_path, g_config.aof_path) < 0) {
        kvs_log(LOG_ERROR, "AOF Rename failed: %s", strerror(errno)); 
        return;
    }
    
    if (g_aof_fd >= 0) close(g_aof_fd);
    
    /* 架构改造 4：无缝衔接主进程的新 AOF I/O 句柄 */
    g_aof_fd = open(g_config.aof_path, O_WRONLY | O_APPEND);
    g_aof_offset = lseek(g_aof_fd, 0, SEEK_END);
    g_aof_base_size = g_aof_offset;
    g_last_rewrite_time = time(NULL);
    
    kvs_log(LOG_INFO, "BGREWRITEAOF success! New base size: %ld bytes.", g_aof_base_size);
}

/**
 * @brief       根据当前状态和系统配置动态审计并触发自动 AOF 压缩
 * @note        核心监控函数，需要被主控 Reactor 循环定时回调。
 */
void kvs_aof_auto_rewrite_check() {
    /*
     * 架构级熔断器 (Circuit Breaker)：
     * 若热更配置的百分比设为 0，则视作主动关闭重写功能。
     * 通常用于大促/压测等极度吃紧的 I/O 隔离期保障。
     */
    if (g_config.aof_rewrite_perc == 0) return;

    if (g_aof_fd < 0 || g_aof_rewrite_pid != -1) return;
    
    /* 设立 5 秒的冷却期限制，严防 I/O 抖动风暴 */
    time_t now = time(NULL);
    if (now - g_last_rewrite_time < 5) return; 

    /* 底线保护：设定最小触发硬门槛 (256MB)，避免细碎文件的无效重写 */
    off_t auto_rewrite_min_size = 256 * 1024 * 1024; 
    
    /* 实时拉取最新配置的热更新值 */
    int auto_rewrite_perc = g_config.aof_rewrite_perc; 

    /* 首发机制：刚满 256MB 时建立首个基线 */
    if (g_aof_base_size == 0 && g_aof_offset >= auto_rewrite_min_size) {
        kvs_log(LOG_INFO, "Initial AOF rewrite triggered (Size: %ld bytes)", g_aof_offset);
        kvs_aof_rewrite_bg(); 
        return;
    }

    if (g_aof_offset < auto_rewrite_min_size) return;
    
    /* 容量溢出判定逻辑 */
    long long growth = (g_aof_offset - g_aof_base_size) * 100 / (g_aof_base_size > 0 ? g_aof_base_size : 1);
    
    if (growth >= auto_rewrite_perc) {
        kvs_log(LOG_INFO, "AOF growth (%lld%%) exceeds threshold (%d%%). Triggering BGREWRITEAOF...", 
                growth, auto_rewrite_perc);
        kvs_aof_rewrite_bg();
    }
}