/**
 * @file        kvs_replication.h
 * @brief       InazumaKV 纯内存主从复制层头文件 (RDMA + XDP 双轨架构)
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        采用现代解耦架构设计的复制状态机 (Replication State Machine)。
 * 彻底告别传统基于 RDB 磁盘 I/O 的全量同步模式。
 * 核心机制：全量同步依赖 RDMA 内存直推实现零拷贝跨机传输；
 * 增量同步依赖 AF_XDP 旁路网络实现微秒级延迟的增量指令回放。
 */

#ifndef __KVS_REPLICATION_H__
#define __KVS_REPLICATION_H__

#include "kvstore.h"

/* ==========================================================================
 * 复制状态机枚举与常量定义 (Replication State Machine)
 * ========================================================================== */

/** @brief 复制状态机：未初始化或处于无连接的独立运行状态 */
#define REPL_STATE_NONE          0

/** @brief 复制状态机：Slave 正在尝试与 Master 建立控制面 TCP 握手 */
#define REPL_STATE_CONNECTING    1

/** @brief 复制状态机：全量同步阶段，正通过 RDMA 硬件通道进行极速内存直推 */
#define REPL_STATE_RDMA_SYNCING  2

/** * @brief 复制状态机：全量同步退避阶段，通过标准 TCP 零拷贝 (Sendfile) 进行回退同步
 * @note  作为 RDMA 链路不可用或未配置时的兜底全量同步方案。 
 */
#define REPL_STATE_TCP_SYNCING   3 

/** @brief 复制状态机：增量同步阶段，正通过 AF_XDP 旁路网关实时接收增量变更流 */
#define REPL_STATE_XDP_REALTIME  4


/* ==========================================================================
 * 全局运行状态与可观测性导出 (Observability)
 * ========================================================================== */

/**
 * @brief 全局复制状态指示器
 * @note  用于记录当前系统在主从复制链路中所处的精确生命周期阶段。
 * 暴露给外部模块，以便于诊断日志打印以及响应客户端的 INFO REPLICATION 遥测指令。
 */
extern int g_repl_state;


/* ==========================================================================
 * 核心主从复制 API (Core Replication APIs)
 * ========================================================================== */

/**
 * @brief       Master 端核心控制钩子：接管并处理 Slave 提交的 PSYNC 命令
 * @param[in]   client_fd  触发 PSYNC 同步请求的 Slave 客户端 TCP 文件描述符
 * @note        该函数一旦触发，Master 将冻结当前的内存基线，
 * 并分配专用的 RDMA Queue Pair (QP)，主动触发物理内存空间的无感直推 (Push) 动作。
 */
void kvs_repl_handle_psync(int client_fd);

/**
 * @brief       Slave 端核心启动钩子：发起与 Master 的全链路复制握手
 * @return      int 0: 握手成功并进入实时同步阶段; -1: 链路建立失败
 * @note        在引擎启动阶段调用。Slave 首先向 Master 发送 PSYNC 协商报文，
 * 随后向本地的网卡注册 RDMA 接收内存池 (Memory Region)，静默等待 Master 的数据轰炸。
 */
int kvs_slave_sync_with_master();

#endif // __KVS_REPLICATION_H__