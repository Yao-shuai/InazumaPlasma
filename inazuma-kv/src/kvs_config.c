/**
 * @file        kvs_config.c
 * @brief       InazumaKV 核心配置加载与热更新模块
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        负责系统默认参数初始化、基于文件系统读取配置文件，
 * 以及对外提供兼容 RESP 协议的 CONFIG GET/SET 热更新接口。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h> // for strcasecmp
#include "kvs_config.h"

/** * @brief 全局配置单例实例，供整个 KVS 引擎读取
 */
kvs_config_t g_config;

/* ==========================================================================
 * 内部辅助函数 (Static Helpers)
 * ========================================================================== */

/**
 * @brief       去除字符串首尾的空白字符
 * @param[in]   str  待处理的原始字符串
 * @return      char* 处理后的字符串首地址
 * @warning     该函数会直接修改传入的字符串内存 (原地替换)，不分配新内存。
 */
static char *trim(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
    
    return str;
}

/**
 * @brief       解析日志级别 (支持字符串字面量或数字回退)
 * @param[in]   val  配置项对应的值字符串
 * @return      kvs_log_level_t 解析出的枚举类型
 */
static kvs_log_level_t parse_log_level(const char *val) {
    if (strcasecmp(val, "trace") == 0) return LOG_TRACE; 
    if (strcasecmp(val, "debug") == 0) return LOG_DEBUG;
    if (strcasecmp(val, "info") == 0)  return LOG_INFO;
    if (strcasecmp(val, "warn") == 0)  return LOG_WARN;
    if (strcasecmp(val, "error") == 0) return LOG_ERROR;
    if (strcasecmp(val, "fatal") == 0) return LOG_FATAL;
    return (kvs_log_level_t)atoi(val); 
}

/**
 * @brief       解析集群节点角色
 * @param[in]   val  配置项对应的值字符串
 * @return      kvs_role_t 解析出的角色枚举
 */
static kvs_role_t parse_role(const char *val) {
    if (strcasecmp(val, "master") == 0) return ROLE_MASTER;
    if (strcasecmp(val, "slave") == 0)  return ROLE_SLAVE;
    return (kvs_role_t)atoi(val);
}

/**
 * @brief       解析持久化模式
 * @param[in]   val  配置项对应的值字符串
 * @return      kvs_persist_mode_t 解析出的持久化枚举
 */
static kvs_persist_mode_t parse_persist_mode(const char *val) {
    if (strcasecmp(val, "none") == 0) return PERSIST_NONE;
    if (strcasecmp(val, "aof") == 0)  return PERSIST_AOF;
    if (strcasecmp(val, "rdb") == 0)  return PERSIST_RDB;
    return (kvs_persist_mode_t)atoi(val);
}


/* ==========================================================================
 * 核心加载与初始化 API (Core Initialization)
 * ========================================================================== */

/**
 * @brief       初始化引擎的全局默认配置
 * @note        在解析本地配置文件前调用，用于提供安全的系统兜底参数。
 */
void kvs_config_init_default() {
    // 网络层默认值
    strncpy(g_config.bind_ip, "0.0.0.0", MAX_IP_LEN - 1);
    g_config.port = 6379; 

    // 集群角色默认值
    g_config.role = ROLE_MASTER;
    strncpy(g_config.master_ip, "127.0.0.1", MAX_IP_LEN - 1);
    g_config.master_port = 6379;

    // eBPF/AF_XDP 旁路网络默认值
    strncpy(g_config.ifname, "eth0", 63); 
    memset(g_config.slave_mac, 0, sizeof(g_config.slave_mac));
    memset(g_config.slave_ip, 0, sizeof(g_config.slave_ip));

    // 灾备与快照默认值 (0 表示默认禁用自动保存)
    g_config.save_interval_ms = 0;      
    g_config.save_changes_limit = 10000;

    // 系统运行基础默认值
    g_config.persistence_mode = PERSIST_RDB; 
    g_config.log_level = LOG_INFO;          
    g_config.aof_rewrite_perc = 100;

    // 持久化文件路径兜底 (基于物理机 bin 目录的相对路径)
    strncpy(g_config.aof_path, "../data/kvs.aof", 255);
    strncpy(g_config.rdb_path, "../data/kvs.rdb", 255);
}

/**
 * @brief       从指定文件加载引擎配置
 * @param[in]   filename  配置文件的相对或绝对路径
 * @return      int       0: 成功加载; -1: 文件打开失败，将使用默认配置
 * @note        支持 '#' 和 ';' 作为行注释，键值对格式为 KEY=VALUE。
 */
int kvs_config_load(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        kvs_log(LOG_WARN, "Could not open %s. Using default settings.", filename);
        return -1;
    }

    char line[1024];
    int line_no = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *start = trim(line);

        // 跳过空行及合法注释标识符
        if (start[0] == '#' || start[0] == ';' || start[0] == '\0') continue;

        char *eq_pos = strchr(start, '=');
        if (!eq_pos) {
            kvs_log(LOG_ERROR, "Syntax error at line %d: Missing '='", line_no);
            continue; 
        }

        // 切割键值对字符串
        *eq_pos = '\0'; 
        char *key = trim(start);
        char *value = trim(eq_pos + 1);

        if (strlen(key) == 0 || strlen(value) == 0) continue;

        // ----------------------------------------
        // 解析挂载配置项 (字典序无感匹配)
        // ----------------------------------------
        if (strcasecmp(key, "bind_ip") == 0) {
            strncpy(g_config.bind_ip, value, MAX_IP_LEN - 1);
        } else if (strcasecmp(key, "port") == 0) {
            int p = atoi(value);
            if (p > 0 && p < 65536) {
                g_config.port = p;
            } else { 
                kvs_log(LOG_ERROR, "Invalid port at line %d.", line_no); 
                exit(EXIT_FAILURE); 
            }
        } else if (strcasecmp(key, "role") == 0) {
            g_config.role = parse_role(value);
        } else if (strcasecmp(key, "master_ip") == 0) {
            strncpy(g_config.master_ip, value, MAX_IP_LEN - 1);
        } else if (strcasecmp(key, "master_port") == 0) {
             int p = atoi(value);
             if (p > 0 && p < 65536) {
                 g_config.master_port = p;
             } else { 
                 kvs_log(LOG_ERROR, "Invalid master_port at line %d.", line_no); 
                 exit(EXIT_FAILURE); 
             }
        } else if (strcasecmp(key, "ifname") == 0) {
            strncpy(g_config.ifname, value, 63);
        } else if (strcasecmp(key, "slave_mac") == 0) {
            strncpy(g_config.slave_mac, value, 31);
        } else if (strcasecmp(key, "slave_ip") == 0) {
            strncpy(g_config.slave_ip, value, MAX_IP_LEN - 1);
        } else if (strcasecmp(key, "persistence_mode") == 0) {
            g_config.persistence_mode = parse_persist_mode(value);
        } else if (strcasecmp(key, "log_level") == 0 || strcasecmp(key, "loglevel") == 0) {
            g_config.log_level = parse_log_level(value);
        } else if (strcasecmp(key, "save_interval_ms") == 0) {
            g_config.save_interval_ms = atoi(value);
        } else if (strcasecmp(key, "save_changes_limit") == 0) {
            g_config.save_changes_limit = atoi(value);
        } else if (strcasecmp(key, "aof_rewrite_percentage") == 0 || strcasecmp(key, "aof-rewrite-percentage") == 0) {
            g_config.aof_rewrite_perc = atoi(value);
        } else if (strcasecmp(key, "aof_path") == 0) {
            strncpy(g_config.aof_path, value, 255);
        } else if (strcasecmp(key, "rdb_path") == 0) {
            strncpy(g_config.rdb_path, value, 255);
        } else {
            kvs_log(LOG_WARN, "Ignoring unknown config key: %s", key); 
        }
    }

    fclose(fp);
    kvs_config_dump(); 
    return 0;
}


/* ==========================================================================
 * 企业级热更新 (Hot Reloading) 接口
 * ========================================================================== */

/**
 * @brief       响应 RESP 客户端的 CONFIG SET 命令，实现核心参数热变更
 * @param[in]   key    需要更新的配置键名
 * @param[in]   value  对应的配置新值
 * @return      int    0: 更新成功; -1: 不支持热更新的键或参数错误
 * @warning     执行热更新不具备线程安全的锁机制，需确保调用链路无数据竞争。
 */
int kvs_config_set(const char *key, const char *value) {
    if (!key || !value) return -1;

    // 1. 动态切换日志级别
    if (strcasecmp(key, "loglevel") == 0 || strcasecmp(key, "log_level") == 0) {
        kvs_log_level_t new_level = parse_log_level(value);
        if (new_level >= LOG_TRACE && new_level <= LOG_FATAL) {
            g_config.log_level = new_level; // 内存瞬间覆盖生效
            kvs_log(LOG_WARN, "[Config] 🚨 Hot Reload: Log level changed to %s", value);
            return 0;
        }
        return -1;
    } 
    // 2. 动态切换 AOF 重写阈值
    else if (strcasecmp(key, "aof-rewrite-percentage") == 0 || strcasecmp(key, "aof_rewrite_percentage") == 0) {
        int new_perc = atoi(value);
        if (new_perc > 0) {
            g_config.aof_rewrite_perc = new_perc; // 内存瞬间覆盖生效
            kvs_log(LOG_WARN, "[Config] 🚨 Hot Reload: AOF rewrite percentage changed to %d%%", new_perc);
            return 0;
        }
        return -1;
    }

    return -1; // 其他键暂时拒绝热更新
}

/**
 * @brief       响应 RESP 客户端的 CONFIG GET 命令，动态查询当前配置
 * @param[in]   key      要查询的配置键名
 * @param[out]  out_buf  底层用于回写的缓冲区
 * @param[in]   max_len  缓冲区的最大安全长度
 * @return      int      写入缓冲区的真实字节数
 * @note        函数直接构造合法的 RESP 协议数组文本返回给调用层，实现了跨层零拷贝拼装。
 */
int kvs_config_get(const char *key, char *out_buf, size_t max_len) {
    if (!key || !out_buf) return 0;

    if (strcasecmp(key, "loglevel") == 0 || strcasecmp(key, "log_level") == 0) {
        const char *lvl_str = "unknown";
        switch (g_config.log_level) {
            case LOG_TRACE: lvl_str = "trace"; break;
            case LOG_DEBUG: lvl_str = "debug"; break;
            case LOG_INFO:  lvl_str = "info";  break;
            case LOG_WARN:  lvl_str = "warn";  break;
            case LOG_ERROR: lvl_str = "error"; break;
            case LOG_FATAL: lvl_str = "fatal"; break;
        }
        return snprintf(out_buf, max_len, "*2\r\n$8\r\nloglevel\r\n$%zu\r\n%s\r\n", strlen(lvl_str), lvl_str);
    } 
    else if (strcasecmp(key, "aof-rewrite-percentage") == 0 || strcasecmp(key, "aof_rewrite_percentage") == 0) {
        char num_buf[32];
        int num_len = snprintf(num_buf, sizeof(num_buf), "%d", g_config.aof_rewrite_perc);
        return snprintf(out_buf, max_len, "*2\r\n$22\r\naof-rewrite-percentage\r\n$%d\r\n%s\r\n", num_len, num_buf);
    }

    // 未知配置返回空数组
    return snprintf(out_buf, max_len, "*0\r\n");
}


/* ==========================================================================
 * 诊断与监控 (Diagnostics & Monitoring)
 * ========================================================================== */

/**
 * @brief       向标准输出打印当前引擎完整的运行时配置清单
 * @note        通常在服务启动完毕后触发，用于运维审计。
 */
void kvs_config_dump() {
    const char *role_str = (g_config.role == ROLE_MASTER) ? "MASTER" : "SLAVE";
    
    const char *persist_str = "NONE";
    if (g_config.persistence_mode == PERSIST_AOF) persist_str = "AOF";
    else if (g_config.persistence_mode == PERSIST_RDB) persist_str = "RDB";

    const char *log_str = "INFO";
    switch (g_config.log_level) {
        case LOG_TRACE: log_str = "TRACE"; break;
        case LOG_DEBUG: log_str = "DEBUG"; break;
        case LOG_INFO:  log_str = "INFO";  break;
        case LOG_WARN:  log_str = "WARN";  break;
        case LOG_ERROR: log_str = "ERROR"; break;
        case LOG_FATAL: log_str = "FATAL"; break;
        default: break;
    }

    printf("========================================\n");
    printf(" 🛡️ InazumaKV Configuration Loaded\n");
    printf("========================================\n");
    printf(" Network:\n");
    printf("   Bind IP:   %s\n", g_config.bind_ip);
    printf("   Port:      %d\n", g_config.port);
    printf(" System:\n");
    printf("   Role:      %s\n", role_str);
    printf("   Log Level: %s\n", log_str);
    printf("   Persist:   %s\n", persist_str);
    
    // 打印内核旁路 (AF_XDP) 关键信息
    printf(" AF_XDP (Kernel Bypass):\n");
    printf("   Interface: %s\n", g_config.ifname);
    
    if (g_config.role == ROLE_MASTER) {
        printf("   Target Slave IP:  %s\n", g_config.slave_ip);
        printf("   Target Slave MAC: %s\n", g_config.slave_mac);
    } else {
        printf(" Replication (TCP Sync):\n");
        printf("   Master IP:   %s\n", g_config.master_ip);
        printf("   Master Port: %d\n", g_config.master_port);
    }

    // 打印容灾落盘配置
    printf(" Disaster Recovery:\n");
    if (g_config.save_interval_ms > 0) {
        printf("   Auto-BGSAVE: %d ms / %d keys\n", g_config.save_interval_ms, g_config.save_changes_limit);
    } else {
        printf("   Auto-BGSAVE: Disabled\n");
    }
    if (g_config.persistence_mode == PERSIST_AOF) {
        printf("   AOF Rewrite: %d%%\n", g_config.aof_rewrite_perc);
    }
    printf("========================================\n");
}