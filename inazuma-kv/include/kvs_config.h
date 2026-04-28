/**
 * @file        kvs_config.h
 * @brief       InazumaKV 核心配置与全局声明头文件
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        定义了系统的全局运行配置结构、集群角色枚举、持久化策略类型。
 * 同时对外暴露了具备热更新能力的宏定义日志组件以及配置加载 API。
 */

#ifndef __KVS_CONFIG_H__
#define __KVS_CONFIG_H__

#include <stdint.h>
#include <stdio.h>
#include <stddef.h> // for size_t

/* ==========================================================================
 * 枚举定义 (Enumerations)
 * ========================================================================== */

/**
 * @brief 节点集群角色枚举
 */
typedef enum {
    ROLE_MASTER = 0,    /**< 集群主节点，负责处理写请求与数据下发 */
    ROLE_SLAVE  = 1     /**< 集群从节点，负责只读请求与数据同步 */
} kvs_role_t;

/**
 * @brief 数据持久化模式枚举
 */
typedef enum {
    PERSIST_NONE = 0,   /**< 纯内存缓存模式，禁用所有落盘行为 */
    PERSIST_AOF  = 1,   /**< Append-Only File，追加事务日志模式 */
    PERSIST_RDB  = 2    /**< Redis Database，定期内存快照模式 */
} kvs_persist_mode_t;

/**
 * @brief 全局日志拦截级别枚举
 */
typedef enum {
    LOG_TRACE = 0,      /**< 极度详细的运行轨迹追踪 */
    LOG_DEBUG = 1,      /**< 调试信息，辅助开发阶段定位问题 */
    LOG_INFO  = 2,      /**< 核心状态与生命周期事件通知 */
    LOG_WARN  = 3,      /**< 警告信息，系统仍在运行但存在潜在风险 */
    LOG_ERROR = 4,      /**< 错误信息，部分功能受损或操作失败 */
    LOG_FATAL = 5       /**< 致命错误，系统即将崩溃或退出 */
} kvs_log_level_t;


/* ==========================================================================
 * 配置结构体 (Configuration Structures)
 * ========================================================================== */

#define MAX_IP_LEN 64

/**
 * @brief 全局配置控制块
 * @note  包含了网络、集群、硬件直通以及灾备等所有核心运行参数。
 * 部分支持热重载的字段 (如 log_level, aof_rewrite_perc) 会在运行时被动态覆写。
 */
typedef struct {
    /* 网络基础配置 */
    char bind_ip[MAX_IP_LEN];   /**< 服务绑定的本地 IP 地址 (默认 0.0.0.0) */
    int port;                   /**< 服务监听的 TCP 端口 */

    /* 集群角色配置 */
    kvs_role_t role;            /**< 当前节点的运行角色 (Master 或 Slave) */
    
    /* 主从复制配置 (仅 Slave 角色有效) */
    char master_ip[MAX_IP_LEN]; /**< 目标 Master 节点的 IP 地址 */
    int master_port;            /**< 目标 Master 节点的 TCP 端口 */

    /* AF_XDP 内核旁路跨机同步专用硬件配置 */
    char ifname[64];            /**< 绑定的底层物理或虚拟网卡名称 (如 eth0) */
    char slave_mac[32];         /**< 目标 Slave 的硬件 MAC 地址，用于直构以太网帧 */
    char slave_ip[MAX_IP_LEN];  /**< 目标 Slave 的 IP 地址，用于直构 IP 报文头 */

    /* 企业级灾备与心跳落盘配置 (Auto-BGSAVE / AOF) */
    int save_interval_ms;       /**< 自动保存的毫秒级间隔时间 (设为 0 表示禁用) */
    int save_changes_limit;     /**< 触发自动落盘的累积数据变更阈值 */
    
    /* AOF 自动重写阈值控制 */
    int aof_rewrite_perc;       /**< 触发 BGREWRITEAOF 的文件体积增长百分比阈值 (支持热重载) */

    /* 持久化文件系统路径分配 */
    char aof_path[256];         /**< AOF 文件的相对或绝对落盘路径 */
    char rdb_path[256];         /**< RDB 文件的相对或绝对落盘路径 */

    /* 系统运行时策略配置 */
    kvs_persist_mode_t persistence_mode; /**< 引擎当前激活的持久化后端模式 */
    kvs_log_level_t log_level;           /**< 动态日志过滤级别门限 */
    
} kvs_config_t;

/* ==========================================================================
 * 全局变量与核心宏声明 (Globals & Macros)
 * ========================================================================== */

/** @brief 全局配置上下文实例，由 kvs_config.c 统一管理与初始化 */
extern kvs_config_t g_config;

/**
 * @brief       标准引擎日志输出宏
 * @param[in]   level 日志级别 (kvs_log_level_t)
 * @param[in]   fmt   格式化字符串及可变参数
 * @note        采用 do-while(0) 封装以保证宏在复杂语句块中的安全展开。
 * 运行时直接判断当前级别与 g_config.log_level 的关系，天生具备热重载感知能力。
 */
#define kvs_log(level, fmt, ...) \
    do { \
        if (level >= g_config.log_level) { \
            const char *level_str = "INFO"; \
            if (level == LOG_TRACE) level_str = "TRACE"; \
            else if (level == LOG_DEBUG) level_str = "DEBUG"; \
            else if (level == LOG_WARN) level_str = "WARN"; \
            else if (level == LOG_ERROR) level_str = "ERROR"; \
            else if (level == LOG_FATAL) level_str = "FATAL"; \
            \
            fprintf(stdout, "[%s] %s:%d: " fmt "\n", \
                    level_str, __FILE__, __LINE__, ##__VA_ARGS__); \
            fflush(stdout); \
        } \
    } while(0)


/* ==========================================================================
 * 对外暴露的配置层 API (Public Configuration APIs)
 * ========================================================================== */

/** @brief 初始化内部内存的默认兜底参数配置 */
void kvs_config_init_default();          

/** * @brief 从指定的本地文件加载配置 
 * @param[in] filename 配置文件路径
 * @return int 0:成功; -1:失败
 */
int kvs_config_load(const char *filename); 

/** @brief 向标准输出打印当前生效的全量参数 (通常用于诊断) */
void kvs_config_dump();                  

/* --------------------------------------------------------------------------
 * 运行时热更新接口 (Hot Reloading API)
 * 支持对接 RESP 协议的 CONFIG SET / CONFIG GET 命令
 * -------------------------------------------------------------------------- */

/**
 * @brief       动态覆写系统配置项
 * @param[in]   key    配置项键名
 * @param[in]   value  配置项新值 (字符串形式)
 * @return      int    0:更新成功; -1:非法参数或不支持热更新的键
 */
int kvs_config_set(const char *key, const char *value);

/**
 * @brief       动态读取当前系统配置项并序列化为 RESP 格式
 * @param[in]   key      配置项键名
 * @param[out]  out_buf  承接 RESP 报文的缓冲区指针
 * @param[in]   max_len  缓冲区的最大安全长度
 * @return      int      写入缓冲区的实际字节数
 */
int kvs_config_get(const char *key, char *out_buf, size_t max_len);

#endif // __KVS_CONFIG_H__