/**
 * @file        kvs_persist.h
 * @brief       InazumaKV 核心持久化引擎头文件
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        提供基于 io_uring 的高性能异步 I/O 接口声明。
 * 包含 AOF (Append-Only File) 事务日志录入、RDB (Redis Database) 内存快照转储，
 * 以及支持 Copy-On-Write 的后台重写 (BGREWRITEAOF / BGSAVE) 控制指令。
 */

#ifndef __KVS_PERSIST_H__
#define __KVS_PERSIST_H__

#include "kvstore.h"

/* ==========================================================================
 * 数据结构与类型标识 (Data Structures & Type Identifiers)
 * ========================================================================== */

/**
 * @brief 底层存储引擎的数据结构类型标识
 * @note  主要用于在 RDB 快照序列化阶段，作为 TLV (Type-Length-Value) 协议的 Type 字段，
 * 以便在反序列化加载时路由至正确的内存数据结构恢复函数。
 */
typedef enum {
    TYPE_ARRAY    = 1,      /**< 数组形态存储引擎 */
    TYPE_RBTREE   = 2,      /**< 红黑树形态存储引擎 */
    TYPE_HASH     = 3,      /**< 哈希表形态存储引擎 */
    TYPE_SKIPLIST = 4       /**< 跳跃表形态存储引擎 */
} kvs_engine_type_t;

/* ==========================================================================
 * 宏定义与全局状态暴露 (Macros & Global States)
 * ========================================================================== */

/**
 * @brief 获取两值中的最大值 (基础比较宏)
 */
#define MAX(a,b) ((a)>(b)?(a):(b))

/** * @brief AOF 后台重写子进程 PID 
 * @note  通过 extern 暴露给事件循环，用于通过 waitpid() 非阻塞监听重写任务的终结状态。
 * 清理了冗余的重复声明，保持外部链接符号的唯一性与整洁性。
 */
extern pid_t g_aof_rewrite_pid;


/* ==========================================================================
 * 生命周期管理接口 (Lifecycle Management APIs)
 * ========================================================================== */

/**
 * @brief       初始化并执行持久化恢复序列 (优先加载 RDB，随后回放 AOF)
 */
void kvs_persist_init();  

/**
 * @brief       启动持久化引擎核心组件 (如 io_uring 队列的初始化及文件句柄映射)
 */
void kvs_persist_start(); 

/**
 * @brief       安全销毁持久化引擎，排空待处理队列并执行文件系统同步 (fsync)
 */
void kvs_persist_stop();  


/* ==========================================================================
 * 核心持久化与 I/O 调度 API (Core Persistence & I/O Dispatch APIs)
 * ========================================================================== */

/**
 * @brief       将原生命令追加至 AOF 环形缓冲队列
 * @param[in]   raw_cmd  待写入的原始命令字符串或 RESP 报文
 * @param[in]   raw_len  命令数据的有效字节长度
 */
void kvs_aof_append(const char *raw_cmd, int raw_len);

/**
 * @brief       基于内存映射 (mmap) 的方式极速加载并反序列化 RDB 文件
 * @param[in]   filename  RDB 数据文件路径
 */
void kvs_rdb_load(const char *filename); 

/**
 * @brief       触发后台 RDB 内存快照落盘 (BGSAVE)
 * @return      int 0: 成功触发 fork; -1: 当前已有后台任务或 fork 失败
 */
int kvs_rdb_bgsave(void);

/**
 * @brief       非阻塞收割内核 io_uring 的完成队列事件 (CQEs)
 * @note        必须在主事件循环中定期调用，以释放底层的 Submission Queue 槽位。
 */
void kvs_persist_reap_completions(void);


/* ==========================================================================
 * AOF 后台重写与事务管理 (AOF Rewrite & Batch Management)
 * ========================================================================== */

/**
 * @brief       触发后台 AOF 文件重写 (BGREWRITEAOF)，压缩冗余事务日志
 * @return      int 0: 成功触发 fork; -1: 当前已有后台重写任务或 fork 失败
 */
int kvs_aof_rewrite_bg(void);

/**
 * @brief       AOF 后台重写完成的回调处理
 * @param[in]   status 子进程退出的状态码
 * @note        负责临时文件的重命名覆盖以及全局文件描述符/偏移量的重置。
 */
void kvs_aof_rewrite_done(int status);

/**
 * @brief       执行 AOF 文件体积的健康度检查
 * @note        依据配置中的 aof_rewrite_perc 比例，决策是否自动触发后台重写。
 */
void kvs_aof_auto_rewrite_check(void);

/**
 * @brief       强制将缓冲区的批处理指令提交至内核 (刷盘)
 * @note        防止环形队列未满导致的数据长尾滞留。
 */
void kvs_aof_batch_submit(void);

#endif // __KVS_PERSIST_H__