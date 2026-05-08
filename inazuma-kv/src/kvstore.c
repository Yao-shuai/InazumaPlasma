/**
 * @file        kvstore.c
 * @brief       InazumaKV 系统总控与生命周期引擎 (Part 1: 核心基础设施与内存拦截层)
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        包含系统头文件依赖、全局状态标识、快速字符串处理原语、
 * 操作系统级信号陷阱 (Signal Handlers)、内存分配钩子以及全局指令路由表声明。
 */

#define _GNU_SOURCE
#include "kvstore.h"

/* 引入持久化模块 */
#include "kvs_persist.h" 

/* 引入主从复制模块 */
#include "kvs_replication.h"

#include "kvs_config.h"
#include "mem_probe.h"
#include "shm_ipc.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h> 
#include <arpa/inet.h>
#include <errno.h>

#if ENABLE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#include <signal.h>
#include <unistd.h> 
#include <sys/wait.h>
#include <malloc.h>
#include "kvs_crash.h" 
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
#include "kvs_vector.h"

/* ==========================================================================
 * 编译器流水线优化宏 (Compiler Pipeline Optimization)
 * ========================================================================== */

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* ==========================================================================
 * 全局运行状态与引擎实例字典 (Global Run-State & Engine Instances)
 * ========================================================================== */

/** @brief 核心事件循环终止信号标志位，volatile 保证对线程与中断的强可见性 */
volatile int g_keep_running = 1;

/** @brief 标识当前系统是否正处于持久化数据冷启动加载阶段 */
int g_is_loading = 0; 

/** @brief 标识当前系统是否成功激活了 AF_XDP 内核旁路网络模式 */
int g_is_xdp_mode = 0;

/** * @brief TCP 控制通道静默位图 (Mute Map) 
 * @note  以 FD 作为索引。当配置为 1 时，协议处理器将强制丢弃该连接的所有输出响应，
 * 用于主从复制链路中阻止反馈风暴。
 */
uint8_t g_muted_fds[65536] = {0};

/* 挂载各大数据结构引擎的全局基址 */
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

extern int kvs_rdb_bgsave(void);
extern int kvs_rdb_save(void);

/* ==========================================================================
 * 极速辅助原语 (High-Performance String Primitives)
 * ========================================================================== */

/**
 * @brief       规避 glibc 开销的极速字符串转整型函数
 * @param[in]   p 目标字符串指针
 * @return      int 转换后的整型数值
 * @note        采用裸指针遍历，剥离了标准库 atoi 的区域设置 (locale) 与复杂锁校验。
 */
static inline int fast_atoi(const char *p) {
    int val = 0;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val * sign;
}

/**
 * @brief       规避 glibc 开销的极速整型转字符串函数
 * @param[in]   val 目标整型数值
 * @param[out]  buf 承接转换结果的字符串缓冲区
 * @return      int 转换生成的字符长度
 */
static inline int fast_itoa(int val, char *buf) {
    if (val == 0) { buf[0] = '0'; return 1; }
    int i = 0, sign = 0;
    unsigned int uval;
    if (val < 0) { sign = 1; buf[0] = '-'; uval = -val; } 
    else { uval = val; }
    
    char temp[32];
    while (uval > 0) {
        temp[i++] = (uval % 10) + '0';
        uval /= 10;
    }
    for (int j = 0; j < i; j++) {
        buf[sign + j] = temp[i - 1 - j];
    }
    return sign + i;
}

/**
 * @brief 快速组装硬编码 RESP 响应报文的辅助宏
 */
#define SET_RESP(resp, str) do { \
    memcpy((resp), (str), sizeof(str) - 1); \
    length = sizeof(str) - 1; \
} while(0)

/* ==========================================================================
 * OS 信号陷阱与优雅退出 (OS Signal Traps & Graceful Shutdown)
 * ========================================================================== */

/**
 * @brief       系统终端/终止信号拦截器 (SIGINT / SIGTERM)
 * @param[in]   signal 捕获到的信号编号
 * @note        通过阻断 g_keep_running，迫使底层 Reactor 循环跳出，
 * 触发后续的 AOF/RDB 刷盘与内存清理流水线。
 */
void sigint_handler(int signal) {
    if (g_keep_running) {
        printf("\n[Signal] Caught signal %d (SIGINT/SIGTERM), shutting down safely...\n", signal);
        g_keep_running = 0; 
    }
}

extern pid_t g_aof_rewrite_pid;
extern pid_t g_rdb_bgsave_pid;

/**
 * @brief       子进程状态变更信号拦截器 (SIGCHLD)
 * @param[in]   signal 捕获到的信号编号
 * @note        采用 WNOHANG 非阻塞模式收割僵尸子进程。
 * 用于侦测后台 RDB 快照 (BGSAVE) 与 AOF 重写 (BGREWRITEAOF) 任务的终结。
 */
void sigchld_handler(int signal) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == g_aof_rewrite_pid) {
            kvs_aof_rewrite_done(status);
        } else if (pid == g_rdb_bgsave_pid) {
            g_rdb_bgsave_pid = -1; 
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                kvs_log(LOG_INFO, "Background RDB child %d finished successfully.", pid);
            } else {
                kvs_log(LOG_ERROR, "Background RDB child %d failed!", pid);
            }
        }
    }
}

/* ==========================================================================
 * 内存分配探针拦截器 (Memory Allocation Probe Hooks)
 * ========================================================================== */

/**
 * @brief       代理系统标准 malloc，执行内存占用量累加遥测
 */
void *kvs_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr != NULL) {
        mem_probe_alloc(malloc_usable_size(ptr)); 
    }
    return ptr;
}

/**
 * @brief       代理系统标准 free，执行内存占用量释放遥测
 */
void kvs_free(void *ptr) {
    if (ptr == NULL) return;
    mem_probe_free(malloc_usable_size(ptr));
    free(ptr);
}

/* ==========================================================================
 * 全局指令字典与枚举映射 (Global Command Dictionary)
 * ========================================================================== */

/**
 * @brief 文本指令静态字典 (引入企业级 INFO 面板与 FAISS 向量检索扩展)
 */
const char *command[] = {
    "SET", "GET", "DEL", "MOD", "EXIST",
    "RSET", "RGET", "RDEL", "RMOD", "REXIST",
    "HSET", "HGET", "HDEL", "HMOD", "HEXIST",
    "ZSET", "ZGET", "ZDEL", "ZMOD", "ZEXIST",
    "SAVE" , "BGSAVE",
    "PSYNC","PING", "COMMAND", "AUTH","CONFIG",
    "BGREWRITEAOF", "INFO", "DBSIZE","VADD", "VSEARCH"
};

/**
 * @brief 指令内部状态机路由枚举
 */
enum {
    KVS_CMD_START = 0,
    KVS_CMD_SET = KVS_CMD_START,
    KVS_CMD_GET,
    KVS_CMD_DEL,
    KVS_CMD_MOD,
    KVS_CMD_EXIST,
    KVS_CMD_RSET,
    KVS_CMD_RGET,
    KVS_CMD_RDEL,
    KVS_CMD_RMOD,
    KVS_CMD_REXIST,
    KVS_CMD_HSET,
    KVS_CMD_HGET,
    KVS_CMD_HDEL,
    KVS_CMD_HMOD,
    KVS_CMD_HEXIST,
    KVS_CMD_ZSET = 15,
    KVS_CMD_ZGET,
    KVS_CMD_ZDEL,
    KVS_CMD_ZMOD,
    KVS_CMD_ZEXIST,
    KVS_CMD_SAVE,
    KVS_CMD_BGSAVE,
    KVS_CMD_PSYNC,
    KVS_CMD_PING,
    KVS_CMD_COMMAND,
    KVS_CMD_AUTH,
    KVS_CMD_CONFIG,
    KVS_CMD_BGREWRITEAOF,
    KVS_CMD_INFO, 
    KVS_CMD_DBSIZE,
    KVS_CMD_VADD,
    KVS_CMD_VSEARCH,
    KVS_CMD_COUNT,  
};

/**
 * @file        kvstore_main.c
 * @brief       InazumaKV 系统总控与生命周期引擎 (Part 2: 双轨复制引擎与网络转发层)
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        包含 Repl Backlog 环形缓冲区机制，以及基于 TCP Sockets 和 
 * AF_XDP 共享内存 (SHM IPC) 的双轨数据面指令透传与微批处理 (Micro-batching) 逻辑。
 */

/* ==========================================================================
 * 工业级双模复制引擎上下文 (Dual-Mode Replication Engine Context)
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * 传统 TCP 模式同步缓冲与状态
 * -------------------------------------------------------------------------- */
/** @brief 分配于 BSS 段的 8MB 静态内存缓冲，用于 TCP 发送前的应用层微批处理合并 */
static char g_slave_wbuffer[8 * 1024 * 1024]; 
/** @brief TCP 缓冲区当前已写入的有效载荷长度 */
static int g_slave_wlen = 0;
/** @brief Master 维护的与 Slave 建立的增量同步 TCP 控制面连接句柄 */
int g_slave_tcp_fd = -1;
/** @brief 标记远端 Slave 是否处于失联死锁状态，避免高频系统调用风暴 */
static int g_slave_dead = 0; 

/* --------------------------------------------------------------------------
 * XDP 共享内存模式 (Kernel Bypass) 零拷贝通信上下文
 * -------------------------------------------------------------------------- */
/** @brief 指向底层无锁 SPSC 环形队列的共享内存基址句柄 */
struct shm_context *g_shm = NULL;
/** @brief 适配底层网络 MTU 大小的暂存合并缓冲区，防止发送大量细碎包降低网卡吞吐 */
static char g_xdp_batch_buf[MAX_PAYLOAD];
/** @brief XDP 缓冲区当前已写入的有效载荷长度 */
static int  g_xdp_batch_len = 0;

/* ==========================================================================
 * 企业级复制积压缓冲区 (Replication Backlog)
 * ========================================================================== */

/** @brief Master 节点当前已生成的全局数据变更字节级逻辑偏移量 */
long long g_master_repl_offset = 0;
/** @brief Slave 节点当前已确认消费的全局数据变更字节级逻辑偏移量 */
long long g_slave_repl_offset = 0;

/** @brief 静态分配的 16MB 环形缓冲区，缓存最近的写指令序列，用于断线快速重连与防丢包重传 */
char g_repl_backlog[16 * 1024 * 1024]; 
/** @brief 复制积压缓冲区的物理最大容量 (16MB) */
long long g_repl_backlog_size = 16 * 1024 * 1024;
/** @brief 环形缓冲区的当前头部写入物理索引 */
long long g_repl_backlog_idx = 0;
/** @brief 环形缓冲区中当前留存的有效历史数据长度 (不超过 g_repl_backlog_size) */
long long g_repl_backlog_histlen = 0;

/** @brief Slave 端用于向 Master 发起 PSYNC 索取重传的控制通道句柄 */
int g_master_tcp_fd = -1; 

/**
 * @brief       将生成的写指令双写压入环形缓冲区，供丢包时 TCP 兜底重传
 * @param[in]   ptr 原始指令内存基址
 * @param[in]   len 原始指令物理长度
 * @note        采用环形队列折返覆盖 (Wrap-around) 算法，确保内存使用恒定。
 */
void feed_repl_backlog(const char *ptr, size_t len) {
    g_master_repl_offset += len;
    size_t space = g_repl_backlog_size - g_repl_backlog_idx;
    
    if (len <= space) {
        memcpy(g_repl_backlog + g_repl_backlog_idx, ptr, len);
    } else {
        /* 处理跨越物理内存边界的拆分折返拷贝 */
        memcpy(g_repl_backlog + g_repl_backlog_idx, ptr, space);
        memcpy(g_repl_backlog, ptr + space, len - space);
    }
    
    g_repl_backlog_idx = (g_repl_backlog_idx + len) % g_repl_backlog_size;
    g_repl_backlog_histlen += len;
    if (g_repl_backlog_histlen > g_repl_backlog_size) {
        g_repl_backlog_histlen = g_repl_backlog_size;
    }
}

/* ==========================================================================
 * 传统 TCP 模式复制引擎 (Legacy TCP Replication Mode)
 * ========================================================================== */

/**
 * @brief       强制将内存中积压的 TCP 微批处理数据推送至内核 Socket 缓冲区
 * @note        采用 MSG_DONTWAIT 非阻塞语义，并在内核缓冲区满时执行严格的失败保护，
 * 避免因 Slave 处理过慢导致 Master 事件循环发生级联雪崩死锁。
 */
void tcp_flush_to_slave() {
    if (g_slave_dead || g_slave_tcp_fd < 0 || g_slave_wlen == 0) return;

    /* 架构防御：抽干控制通道中潜在的冗余脏响应，防止双端 Socket 缓冲区被填满而引发交叉死锁 */
    char drain_buf[8192];
    while (recv(g_slave_tcp_fd, drain_buf, sizeof(drain_buf), MSG_DONTWAIT) > 0);

    int total_sent = 0;
    while (total_sent < g_slave_wlen) {
        int ret = send(g_slave_tcp_fd, g_slave_wbuffer + total_sent, g_slave_wlen - total_sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (ret > 0) {
            total_sent += ret;
        } else if (ret < 0) {
            if (errno == EINTR) continue;
            
            /* 核心流控：发送窗口拥塞时立即中止。残余数据将安全截留在用户态 8MB 内存池中 */
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; 
            
            /* 连接断开或重置处理 */
            close(g_slave_tcp_fd); 
            g_slave_tcp_fd = -1; 
            g_slave_dead = 1; 
            g_slave_wlen = 0; 
            return;
        }
    }

    /* 内存页整理：将因拥塞未发完的残余数据段通过 memmove 平移至缓冲区头部，等待下次积攒发送 */
    if (total_sent > 0 && total_sent < g_slave_wlen) {
        memmove(g_slave_wbuffer, g_slave_wbuffer + total_sent, g_slave_wlen - total_sent);
    }
    g_slave_wlen -= total_sent;
}

/**
 * @brief       接收原始数据流并将其合并至 TCP 发送队列
 * @param[in]   raw_cmd 原始指令内存基址
 * @param[in]   raw_len 原始指令物理长度
 * @note        此函数将触发与 Slave 节点的建连。同时应用了微批处理设计，
 * 彻底移除了每条命令触发 flush 的反模式，消除了高频的系统调用开销。
 */
void tcp_forward_to_slave(const char *raw_cmd, int raw_len) {
    if (g_slave_dead) return; 

    if (g_slave_tcp_fd < 0) {
        g_slave_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
        
        /* 阻塞式建连，确保 3 次握手绝对完成 */
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(6380); 
        if (strlen(g_config.slave_ip) > 0) {
            inet_pton(AF_INET, g_config.slave_ip, &addr.sin_addr); 
        } else {
            inet_pton(AF_INET, "192.168.124.14", &addr.sin_addr); 
        }
        
        if (connect(g_slave_tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(g_slave_tcp_fd); 
            g_slave_tcp_fd = -1; 
            g_slave_dead = 1; 
            kvs_log(LOG_ERROR, "Failed to establish TCP fallback channel!");
            return; 
        }

        /* 建连成功后，再切回 O_NONBLOCK 保证性能 */
        int flags = fcntl(g_slave_tcp_fd, F_GETFL, 0);
        fcntl(g_slave_tcp_fd, F_SETFL, flags | O_NONBLOCK);

        int nodelay = 1;
        setsockopt(g_slave_tcp_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        const char *mute_cmd = "*1\r\n$9\r\nREPL_MUTE\r\n";
        send(g_slave_tcp_fd, mute_cmd, strlen(mute_cmd), 0);
        
        kvs_log(LOG_INFO, "TCP Fallback Channel established for large payloads!");
    }
    
    /* 抽干可能的脏响应报文 */
    char drain_buf[4096];
    while (recv(g_slave_tcp_fd, drain_buf, sizeof(drain_buf), MSG_DONTWAIT) > 0);

    /* 容量控制保护：仅当 8MB 内存池濒临溢出边界时，强制触发同步 Flush */
    if (g_slave_wlen > 0 && g_slave_wlen + raw_len > sizeof(g_slave_wbuffer)) {
        tcp_flush_to_slave();
        /* 若 Flush 执行后可用空间依然不足，说明网络严重阻塞或断连，主动放弃当前指令防宕机 */
        if (g_slave_wlen + raw_len > sizeof(g_slave_wbuffer)) {
            kvs_log(LOG_ERROR, "Slave TCP buffer overflow! Disconnecting slave.");
            close(g_slave_tcp_fd); 
            g_slave_tcp_fd = -1; 
            g_slave_dead = 1;
            return;
        }
    }

    if (!g_slave_dead) {
        /* 应用层缓冲池微批合并 */
        memcpy(g_slave_wbuffer + g_slave_wlen, raw_cmd, raw_len);
        g_slave_wlen += raw_len;
    }
}

/* ==========================================================================
 * XDP 零拷贝共享内存通信层 (XDP Zero-Copy SHM IPC Plane)
 * ========================================================================== */

/**
 * @brief 初始化 XDP 独立网关的共享内存通信通道
 */
void kvs_xdp_ipc_init() { 
    printf("[KV Engine] Initializing Shared Memory for XDP Gateway...\n");
    /* 业务进程恒定扮演共享内存的创建者角色 */
    g_shm = shm_ipc_init(true);
    if (!g_shm) {
        fprintf(stderr, "[Fatal] Failed to init SHM for XDP!\n");
        exit(EXIT_FAILURE); 
    }
}

/**
 * @brief       将暂存合并的 XDP 微批处理数据帧推送入共享内存发送队列
 * @note        采用严厉的防阻塞降级策略。一旦 SPSC 队列资源耗尽，直接抛弃报文退还 CPU 时间片。
 * 后续的强一致性将由 Replica Backlog 以及 TCP PING 重传机制提供最终兜底。
 */
void xdp_flush_to_slave() {
    if (!g_shm || g_xdp_batch_len == 0) return;

    /* 致命保护逻辑：入队失败代表网关离线或极度拥塞。立即执行 Drop 逻辑，杜绝主进程自旋死锁 */
    if (!spsc_enqueue(&g_shm->tx_queue, g_xdp_batch_buf, g_xdp_batch_len)) {
        g_xdp_batch_len = 0;
        return;
    }
    g_xdp_batch_len = 0;
}

/**
 * @brief       零拷贝提取原生协议帧并透传至 XDP 网关共享内存
 * @param[in]   raw_cmd 原始指令内存基址
 * @param[in]   raw_len 原始指令物理长度
 * @note        对协议帧进行基于 MTU 的智能组装拼装，以最高效地利用网卡帧间隙 (Inter-Frame Gap)。
 */
void xdp_forward_to_slave(const char *raw_cmd, int raw_len) {
    if (!g_shm || raw_cmd == NULL || raw_len <= 0) return;

    /* 防御超常尺寸的报文 (罕见情况，超过单帧 MTU 载荷界限) */
    if (raw_len > MAX_PAYLOAD) {
        xdp_flush_to_slave();
        /* 针对巨型报文直接尝试注入队列，若空间不足立即丢弃以防阻塞 */
        if (!spsc_enqueue(&g_shm->tx_queue, raw_cmd, raw_len)) {
            return;
        }
        return;
    }

    /* MTU 容量阈值审计：若汇入新报文将导致越界，则提前驱逐现存数据 */
    if (g_xdp_batch_len + raw_len > MAX_PAYLOAD) {
        xdp_flush_to_slave();
    }

    /* 执行内核态旁路之前的最后一次用户态数据拼装 */
    memcpy(g_xdp_batch_buf + g_xdp_batch_len, raw_cmd, raw_len);
    g_xdp_batch_len += raw_len;
    
    /* 达到以太网 MTU 最佳水位线阈值 (1350 Bytes)，主动触发队列写入 */
    if (g_xdp_batch_len >= 1350) {
        xdp_flush_to_slave();
    }
}
/* ==========================================================================
 * 零拷贝协议解析分词器 (Zero-Copy Protocol Parsers)
 * ========================================================================== */

/**
 * @brief       标准 RESP (Redis Serialization Protocol) 协议解析器
 * @param[in]   msg    原始网络接收缓冲区的基址
 * @param[in]   length 缓冲区内有效数据的物理长度
 * @param[out]  tokens 承接解析结果的定长零拷贝 Token 数组
 * @return      int    实际成功消耗解析的字节数。若数据不完整则返回 0，协议错误返回 -1。
 * @note        采用纯指针偏移运算，禁止任何形式的数据拷贝与内存分配。
 * 提取的数据分片全部表现为指向原始缓冲区的裸指针加偏移长度。
 */
int kvs_parser_resp(char *msg, int length, kvs_token_t *tokens) {
    if (length < 4) return 0; 
    char *p = msg;
    char *end = msg + length;
    
    if (*p != '*') return -1;
    p++;
    
    int argc = 0;
    while (*p >= '0' && *p <= '9') { argc = argc * 10 + (*p - '0'); p++; }
    p += 2; 
    
    if (argc > KVS_MAX_TOKENS) return -1;

    for (int i = 0; i < argc; i++) {
        if (p >= end) return 0;
        if (*p != '$') return -1;
        p++;
        
        int vlen = 0;
        while (*p >= '0' && *p <= '9') { vlen = vlen * 10 + (*p - '0'); p++; }
        p += 2; 
        
        if (p + vlen + 2 > end) return 0; 
        
        tokens[i].ptr = p;
        tokens[i].len = vlen;
        
        /* 架构约束：禁止在此处修改原始报文 (如追加 \0)，以保障后续落盘与旁路转发的二进制安全 */
        p += vlen + 2; 
    }
    
    if (argc < KVS_MAX_TOKENS) tokens[argc].ptr = NULL;
    return (int)(p - msg); 
}

/**
 * @brief       传统内联 (Inline/Telnet) 文本协议解析器
 * @param[in]   msg    原始网络接收缓冲区的基址 (将被 strtok_r 修改)
 * @param[out]  tokens 承接解析结果的 Token 数组
 * @return      int    解析提取的参数个数
 */
int kvs_parser_inline(char *msg, kvs_token_t *tokens) {
    if (msg == NULL || tokens == NULL) return 0;

    int idx = 0;
    char *saveptr = NULL;

    char *token = strtok_r(msg, " ", &saveptr);
    if (!token) return 0;
    tokens[idx].ptr = token;
    tokens[idx].len = strlen(token);
    idx++;

    token = strtok_r(NULL, " ", &saveptr);
    if (token) {
        tokens[idx].ptr = token;
        tokens[idx].len = strlen(token);
        idx++;

        char *rest = saveptr;
        while (rest && *rest == ' ') rest++;
        if (rest && *rest != '\0') {
            size_t len = strlen(rest);
            if (len > 0 && rest[len-1] == '\r') {
                rest[len-1] = '\0';
                len--;
            }
            tokens[idx].ptr = rest;
            tokens[idx].len = len;
            idx++;
        }
    }
    
    if (idx < KVS_MAX_TOKENS) {
        tokens[idx].ptr = NULL;
    }
    return idx;
}

/* ==========================================================================
 * 业务指令执行引擎与持久化/复制钩子 (Executor & Interceptors)
 * ========================================================================== */

/** @brief 配置常量：触发自动快照落盘的修改操作累积阈值 (0 表示关闭此功能) */
#define AUTO_SAVE_THRESHOLD 0
/** @brief 配置常量：决定自动快照是否采用异步派生模式 (BGSAVE) */
#define USE_BGSAVE_FOR_AUTO 1

/**
 * @brief       中央执行器：分发业务指令并触发持久化与主从复制拦截器
 * @param[in]   client_fd 通信对端的 TCP 描述符 (-1 代表正在执行恢复重放操作)
 * @param[in]   tokens    经过词法分析的零拷贝参数结构集
 * @param[in]   count     指令的有效参数总量
 * @param[out]  response  承接结果输出的预置网络缓冲区
 * @param[in]   raw_cmd   原生态的完整入站报文 (用于零拷贝转发至 AOF / Replica)
 * @param[in]   raw_len   入站报文的精确物理长度
 * @return      int       装填至 response 缓冲区的最终有效字节数
 */
int kvs_executor(int client_fd, kvs_token_t *tokens, int count, char *response, const char *raw_cmd, int raw_len) {
    if (tokens == NULL || count <= 0 || tokens[0].ptr == NULL) return 0; 

    int cmd = KVS_CMD_COUNT;
    int cmd_len = tokens[0].len;
    char *p = tokens[0].ptr;
    /* 通过位或 0x20 运算，实现 ASCII 字母的极速统一小写化，规避库函数调用 */
    char c0 = p[0] | 0x20; 

    /* --------------------------------------------------------------------------
     * 极速命令路由树 (Fast-Path Command Routing based on Length Matrix)
     * -------------------------------------------------------------------------- */
    if (cmd_len == 4) {
        if (c0 == 'h' && (p[1]|0x20) == 's' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 't') cmd = KVS_CMD_HSET;
        else if (c0 == 'h' && (p[1]|0x20) == 'g' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 't') cmd = KVS_CMD_HGET;
        else if (c0 == 'h' && (p[1]|0x20) == 'd' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 'l') cmd = KVS_CMD_HDEL;
        else if (c0 == 'h' && (p[1]|0x20) == 'm' && (p[2]|0x20) == 'o' && (p[3]|0x20) == 'd') cmd = KVS_CMD_HMOD;

        else if (c0 == 'z' && (p[1]|0x20) == 's' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 't') cmd = KVS_CMD_ZSET;
        else if (c0 == 'z' && (p[1]|0x20) == 'g' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 't') cmd = KVS_CMD_ZGET; 
        else if (c0 == 'z' && (p[1]|0x20) == 'd' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 'l') cmd = KVS_CMD_ZDEL; 
        else if (c0 == 'z' && (p[1]|0x20) == 'm' && (p[2]|0x20) == 'o' && (p[3]|0x20) == 'd') cmd = KVS_CMD_ZMOD; 

        else if (c0 == 'r' && (p[1]|0x20) == 's' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 't') cmd = KVS_CMD_RSET; 
        else if (c0 == 'r' && (p[1]|0x20) == 'g' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 't') cmd = KVS_CMD_RGET; 
        else if (c0 == 'r' && (p[1]|0x20) == 'd' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 'l') cmd = KVS_CMD_RDEL; 
        else if (c0 == 'r' && (p[1]|0x20) == 'm' && (p[2]|0x20) == 'o' && (p[3]|0x20) == 'd') cmd = KVS_CMD_RMOD; 

        else if (c0 == 's' && (p[1]|0x20) == 'a' && (p[2]|0x20) == 'v' && (p[3]|0x20) == 'e') cmd = KVS_CMD_SAVE;
        else if (c0 == 'p' && (p[1]|0x20) == 'i' && (p[2]|0x20) == 'n' && (p[3]|0x20) == 'g') cmd = KVS_CMD_PING; 
        else if (c0 == 'a' && (p[1]|0x20) == 'u' && (p[2]|0x20) == 't' && (p[3]|0x20) == 'h') cmd = KVS_CMD_AUTH; 
        else if (c0 == 'i' && (p[1]|0x20) == 'n' && (p[2]|0x20) == 'f' && (p[3]|0x20) == 'o') cmd = KVS_CMD_INFO;
        
        else if (c0 == 'v' && (p[1]|0x20) == 'a' && (p[2]|0x20) == 'd' && (p[3]|0x20) == 'd') cmd = KVS_CMD_VADD;

    } else if (cmd_len == 3) {
        if (c0 == 's' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 't') cmd = KVS_CMD_SET;
        else if (c0 == 'g' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 't') cmd = KVS_CMD_GET;
        else if (c0 == 'd' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 'l') cmd = KVS_CMD_DEL;
        else if (c0 == 'm' && (p[1]|0x20) == 'o' && (p[2]|0x20) == 'd') cmd = KVS_CMD_MOD;

    } else if (cmd_len == 5) {
        if (c0 == 'p' && (p[1]|0x20) == 's' && (p[2]|0x20) == 'y' && (p[3]|0x20) == 'n' && (p[4]|0x20) == 'c') cmd = KVS_CMD_PSYNC; 
        else if (c0 == 'e' && (p[1]|0x20) == 'x' && (p[2]|0x20) == 'i' && (p[3]|0x20) == 's' && (p[4]|0x20) == 't') cmd = KVS_CMD_EXIST;
    } else if (cmd_len == 6) {
        if (c0 == 'h' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 'x' && (p[3]|0x20) == 'i') cmd = KVS_CMD_HEXIST;
        else if (c0 == 'z' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 'x' && (p[3]|0x20) == 'i') cmd = KVS_CMD_ZEXIST; 
        else if (c0 == 'r' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 'x' && (p[3]|0x20) == 'i') cmd = KVS_CMD_REXIST; 
        else if (c0 == 'c' && (p[1]|0x20) == 'o' && (p[2]|0x20) == 'n' && (p[3]|0x20) == 'f') cmd = KVS_CMD_CONFIG;
        else if (c0 == 'b' && (p[1]|0x20) == 'g' && (p[2]|0x20) == 's' && (p[3]|0x20) == 'a') cmd = KVS_CMD_BGSAVE;
        else if (c0 == 'd' && (p[1]|0x20) == 'b' && (p[2]|0x20) == 's' && (p[3]|0x20) == 'i' && (p[4]|0x20) == 'z' && (p[5]|0x20) == 'e') cmd = KVS_CMD_DBSIZE;

    } else if (cmd_len == 7) {
        if (c0 == 'c' && (p[1]|0x20) == 'o' && (p[2]|0x20) == 'm' && (p[3]|0x20) == 'm') cmd = KVS_CMD_COMMAND; 
        else if (c0 == 'v' && (p[1]|0x20) == 's' && (p[2]|0x20) == 'e' && (p[3]|0x20) == 'a' && (p[4]|0x20) == 'r' && (p[5]|0x20) == 'c' && (p[6]|0x20) == 'h') cmd = KVS_CMD_VSEARCH;

    } else if (cmd_len == 9) {
        if (c0 == 'r' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 'p' && (p[3]|0x20) == 'l') {
            if (strncasecmp(p, "REPL_MUTE", 9) == 0) {
                if (client_fd >= 0 && client_fd < 65536) {
                    g_muted_fds[client_fd] = 1;
                }
                memcpy(response, "+OK\r\n", 5);
                return 5;
            }
        }
    } else if (cmd_len == 12) {
        if (c0 == 'b' && (p[1]|0x20) == 'g' && (p[2]|0x20) == 'r' && (p[3]|0x20) == 'e') cmd = KVS_CMD_BGREWRITEAOF; 
    }

    int length = 0;
    int ret = 0;
    char *key = (count > 1) ? tokens[1].ptr : NULL;
    char *value = (count > 2) ? tokens[2].ptr : NULL;
    int key_len = (count > 1) ? tokens[1].len : 0;
    int val_len = (count > 2) ? tokens[2].len : 0;
    
    /* 数据修改指示灯：指示该指令是否改变了系统内存状态，用于驱动持久化/复制旁路 */
    int is_write_success = 0; 

    /* --------------------------------------------------------------------------
     * 指令执行分发层 (Command Execution Dispatcher)
     * -------------------------------------------------------------------------- */
    switch(cmd) {
#if ENABLE_ARRAY
    case KVS_CMD_SET:
        if (!key || !value) {
            SET_RESP(response, "-ERR wrong number of arguments\r\n");
        } else {
            ret = kvs_array_set(&global_array, key, key_len, value, val_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, "+OK\r\n"); is_write_success = 1; }
            else { SET_RESP(response, "+EXIST\r\n"); is_write_success = 1; }
        }
        break;
        
    case KVS_CMD_GET: 
        if (!key) {
            SET_RESP(response, "-ERR wrong number of arguments\r\n");
        } else {
            int out_vlen = 0;
            char *result = kvs_array_get(&global_array, key, key_len, &out_vlen);
            if (result == NULL) {
                SET_RESP(response, "$-1\r\n");
            } else {
                char *p = response;
                *p++ = '$'; p += fast_itoa(out_vlen, p); *p++ = '\r'; *p++ = '\n';
                memcpy(p, result, out_vlen); p += out_vlen; *p++ = '\r'; *p++ = '\n';
                length = p - response;
            }
        }
        break;
    
    case KVS_CMD_DEL:
        if (!key) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_array_del(&global_array, key, key_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, ":1\r\n"); is_write_success = 1; }
            else SET_RESP(response, ":0\r\n");
        }
        break;

    case KVS_CMD_MOD:
        if (!key || !value) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_array_mod(&global_array, key, key_len, value, val_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, "+OK\r\n"); is_write_success = 1; }
            else SET_RESP(response, "-ERR Key not found\r\n");
        }
        break;

    case KVS_CMD_EXIST:
        if (!key) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_array_exist(&global_array, key, key_len);
            if (ret == 0) SET_RESP(response, ":1\r\n"); 
            else SET_RESP(response, ":0\r\n");
        }
        break;
#endif

#if ENABLE_RBTREE
    case KVS_CMD_RSET:
        if (!key || !value) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
           ret = kvs_rbtree_set(&global_rbtree, key, key_len, value, val_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, "+OK\r\n"); is_write_success = 1; }
            else { SET_RESP(response, "+EXIST\r\n"); is_write_success = 1; }
        }
        break;
        
    case KVS_CMD_RGET: 
        if (!key) SET_RESP(response, "-ERR\r\n");
        else {
            int out_vlen = 0;
            char *result = kvs_rbtree_get(&global_rbtree, key, key_len, &out_vlen);
            if (result == NULL) SET_RESP(response, "$-1\r\n");
            else {
                char *p = response;
                *p++ = '$'; p += fast_itoa(out_vlen, p); *p++ = '\r'; *p++ = '\n';
                memcpy(p, result, out_vlen); p += out_vlen; *p++ = '\r'; *p++ = '\n';
                length = p - response;
            }
        }
        break;
        
    case KVS_CMD_RDEL:
        if (!key) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_rbtree_del(&global_rbtree, key, key_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, ":1\r\n"); is_write_success = 1; }
            else SET_RESP(response, ":0\r\n");
        }
        break;
        
    case KVS_CMD_RMOD:
        if (!key || !value) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_rbtree_mod(&global_rbtree, key, key_len, value, val_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, "+OK\r\n"); is_write_success = 1; }
            else SET_RESP(response, "-ERR Key not found\r\n");
        }
        break;
        
    case KVS_CMD_REXIST:
        if (!key) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_rbtree_exist(&global_rbtree, key, key_len);
            if (ret == 0) SET_RESP(response, ":1\r\n"); 
            else SET_RESP(response, ":0\r\n");
        }
        break;
#endif

#if ENABLE_HASH
    case KVS_CMD_HSET:
        if (!key || !value) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_hash_set(&global_hash, key, key_len, value, val_len);
            
            /* 如果是 AOF 重放时发现了 VEC: 前缀的影子 Key，同步恢复到 FAISS */
            if (key_len > 4 && strncmp(key, "VEC:", 4) == 0) {
                if (val_len % sizeof(float) == 0) {
                    kvs_vector_add(key + 4, key_len - 4, (float*)value);
                }
            }
            
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, "+OK\r\n"); is_write_success = 1; }
            else { SET_RESP(response, "+EXIST\r\n"); is_write_success = 1; }
        }
        break;
        
    case KVS_CMD_HGET: 
        if (!key) SET_RESP(response, "-ERR\r\n");
        else {
            int out_vlen = 0;
            char *result = kvs_hash_get(&global_hash, key, key_len, &out_vlen);
            if (result == NULL) SET_RESP(response, "$-1\r\n");
            else {
                char *p = response;
                *p++ = '$'; p += fast_itoa(out_vlen, p); *p++ = '\r'; *p++ = '\n';
                memcpy(p, result, out_vlen); p += out_vlen; *p++ = '\r'; *p++ = '\n';
                length = p - response;
            }
        }
        break;
        
    case KVS_CMD_HDEL:
        if (!key) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_hash_del(&global_hash, key, key_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, ":1\r\n"); is_write_success = 1; }
            else SET_RESP(response, ":0\r\n");
        }
        break;
        
    case KVS_CMD_HMOD:
        if (!key || !value) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_hash_mod(&global_hash, key, key_len, value, val_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, "+OK\r\n"); is_write_success = 1; }
            else SET_RESP(response, "-ERR Key not found\r\n");
        }
        break;
        
    case KVS_CMD_HEXIST:
        if (!key) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_hash_exist(&global_hash, key, key_len);
            if (ret == 0) SET_RESP(response, ":1\r\n"); 
            else SET_RESP(response, ":0\r\n");
        }
        break;
#endif

#if ENABLE_SKIPLIST
    case KVS_CMD_ZSET:
        if (!key || !value) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_skiplist_set(&global_skiplist, key, key_len, value, val_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, "+OK\r\n"); is_write_success = 1; }
            else { SET_RESP(response, "+EXIST\r\n"); is_write_success = 1; }
        }
        break;
        
    case KVS_CMD_ZGET: 
        if (!key) SET_RESP(response, "-ERR\r\n");
        else {
            int out_vlen = 0;
            char *result = kvs_skiplist_get(&global_skiplist, key, key_len, &out_vlen);
            if (result == NULL) SET_RESP(response, "$-1\r\n");
            else {
                char *p = response;
                *p++ = '$'; p += fast_itoa(out_vlen, p); *p++ = '\r'; *p++ = '\n';
                memcpy(p, result, out_vlen); p += out_vlen; *p++ = '\r'; *p++ = '\n';
                length = p - response;
            }
        }
        break;
        
    case KVS_CMD_ZDEL:
        if (!key) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_skiplist_del(&global_skiplist,key,key_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, ":1\r\n"); is_write_success = 1; }
            else SET_RESP(response, ":0\r\n");
        }
        break;
        
    case KVS_CMD_ZMOD:
        if (!key || !value) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_skiplist_mod(&global_skiplist,key,key_len,value,val_len);
            if (ret < 0) SET_RESP(response, "-ERR Server Error\r\n");
            else if (ret == 0) { SET_RESP(response, "+OK\r\n"); is_write_success = 1; }
            else SET_RESP(response, "-ERR Key not found\r\n");
        }
        break;
        
    case KVS_CMD_ZEXIST:
        if (!key) SET_RESP(response, "-ERR wrong number of arguments\r\n");
        else {
            ret = kvs_skiplist_exist(&global_skiplist,key,key_len);
            if (ret == 0) SET_RESP(response, ":1\r\n"); 
            else SET_RESP(response, ":0\r\n");
        }
        break;
#endif

    case KVS_CMD_INFO:
        /* 商业级监控探针：将内存大屏状态转化为 Prometheus 格式返回，越过后续任何拦截器 */
        length = mem_probe_generate_info_memory(response, KVS_MAX_RSP_LEN);
        return length; 

    case KVS_CMD_SAVE:
        kvs_rdb_save();
        SET_RESP(response, "+OK\r\n");
        break;

    case KVS_CMD_BGSAVE:
        extern pid_t g_rdb_bgsave_pid; 
        if (g_rdb_bgsave_pid != -1) {
            SET_RESP(response, "+Background saving already in progress\r\n");
            break;
        }

        if (kvs_rdb_bgsave() == 0) {
            SET_RESP(response, "+Background saving started\r\n");
        } else {
            SET_RESP(response, "-ERR Background save failed\r\n");
        }
        break;
    
    case KVS_CMD_PING:
       if (count > 1 && g_config.role == ROLE_SLAVE) {
            long long pkt_offset = atoll(tokens[1].ptr);
            if (pkt_offset > g_slave_repl_offset) {
                kvs_log(LOG_WARN, "[Slave] XDP packet drop detected! Expected Offset: %lld, Received: %lld. Initiating TCP NACK retransmission.", 
                        g_slave_repl_offset, pkt_offset);
                
                if (g_master_tcp_fd != -1) {
                    char psync_cmd[128];
                    int len = sprintf(psync_cmd, "*2\r\n$5\r\nPSYNC\r\n$%d\r\n%lld\r\n", 
                  snprintf(NULL, 0, "%lld", g_slave_repl_offset), g_slave_repl_offset);
                    send(g_master_tcp_fd, psync_cmd, len, 0);
                }
            }
        }
        SET_RESP(response, "+PONG\r\n");
        break;
    
    case KVS_CMD_COMMAND:
        SET_RESP(response, "+OK\r\n"); 
        break;
    
    case KVS_CMD_CONFIG:
        if (count >= 3) {
            char *subcmd = tokens[1].ptr;
            int subcmd_len = tokens[1].len; 
            char *param = tokens[2].ptr;
            int param_len = tokens[2].len;  

            if (subcmd_len == 3 && strncasecmp(subcmd, "SET", 3) == 0 && count >= 4) {
                /* Reserved for dynamic configuration tuning logic */
            } 
            else if (subcmd_len == 3 && strncasecmp(subcmd, "GET", 3) == 0) {
                /* * 架构兼容层：拦截特定监控指令，伪造 Redis 官方格式，
                 * 无缝对接 redis-benchmark 测试基准 
                 */
                if ((param_len == 1 && param[0] == '*') || 
                    (param_len == 4 && strncasecmp(param, "save", 4) == 0) || 
                    (param_len == 10 && strncasecmp(param, "appendonly", 10) == 0)) {
                    
                    static const char fake_config[] = "*4\r\n"
                                                      "$4\r\nsave\r\n$0\r\n\r\n"
                                                      "$10\r\nappendonly\r\n$2\r\nno\r\n";
                    length = sizeof(fake_config) - 1; 
                    memcpy(response, fake_config, length);
                    return length;
                }

                /* 兜底动态配置输出 */
                length = snprintf(response, KVS_MAX_RSP_LEN, "*2\r\n$%d\r\n%.*s\r\n$0\r\n\r\n", param_len, param_len, param);
                return length;
            } else {
                SET_RESP(response, "-ERR Syntax error for CONFIG command\r\n");
            }
        } else {
            SET_RESP(response, "-ERR wrong number of arguments for CONFIG\r\n");
        }
        break;
        
    case KVS_CMD_AUTH:
        SET_RESP(response, "+OK\r\n");
        break;
        
    case KVS_CMD_PSYNC:
        if (g_config.role == ROLE_MASTER) {
            g_slave_dead = 0;        
            g_slave_wlen = 0;        
            if (g_slave_tcp_fd != -1) {
                close(g_slave_tcp_fd);
                g_slave_tcp_fd = -1; 
            }
            kvs_log(LOG_INFO, "[Master] Slave reconnected. Synchronization locks lifted, legacy buffers purged.");
            
            /* 增量同步断点续传逻辑解析：PSYNC <offset> */
            if (count > 1) { 
                long long req_offset = atoll(tokens[1].ptr);
                /* 判断 Slave 索要的游标是否仍被缓存在我们的环形 Backlog 中 */
                if (g_repl_backlog_histlen > 0 && 
                    req_offset >= g_master_repl_offset - g_repl_backlog_histlen && 
                    req_offset <= g_master_repl_offset) {
                    
                    kvs_log(LOG_INFO, "[Master] Activating TCP retrospective fallback. Retransmitting from offset %lld to %lld", req_offset, g_master_repl_offset);
                    
                    long long diff = g_master_repl_offset - req_offset;
                    long long start_idx = (g_repl_backlog_idx - diff + g_repl_backlog_size) % g_repl_backlog_size;
                    
                    if (start_idx + diff <= g_repl_backlog_size) {
                        send(client_fd, g_repl_backlog + start_idx, diff, 0);
                    } else { 
                        /* 跨越物理边界的折返发射序列 */
                        long long part1 = g_repl_backlog_size - start_idx;
                        send(client_fd, g_repl_backlog + start_idx, part1, 0);
                        send(client_fd, g_repl_backlog, diff - part1, 0);
                    }
                    return 0; 
                }
            }
            /* 后退保护：若游标过期被覆写，主动降级为完整的内存全量快照 (RDB) 派发 */
            kvs_log(LOG_WARN, "[Master] Replication backlog insufficient. Falling back to full memory snapshot sync...");
            extern void kvs_repl_handle_psync(int client_fd);
            kvs_repl_handle_psync(client_fd);
            return 0; 
        }
        break;
        
    case KVS_CMD_BGREWRITEAOF:
        if (kvs_aof_rewrite_bg() == 0) {
            SET_RESP(response, "+Background append only file rewriting started\r\n");
        } else {
            SET_RESP(response, "-ERR Background append only file rewriting failed\r\n");
        }
        break;
        
    case KVS_CMD_VADD:
        if (count < 3 || tokens[2].len % sizeof(float) != 0) {
            SET_RESP(response, "-ERR invalid vector binary data length\r\n");
        } else {
            /* 1. 存入 C++ 向量引擎 */
            kvs_vector_add(key, key_len, (float*)tokens[2].ptr);
            
            /* 2. 加前缀存入哈希表！防止和文本 HSET 键名冲突 */
            char shadow_key[256];
            
            int shadow_klen = snprintf(shadow_key, sizeof(shadow_key), "VEC:%.*s", key_len, key);
            
            kvs_hash_set(&global_hash, shadow_key, shadow_klen, tokens[2].ptr, tokens[2].len);

            SET_RESP(response, "+OK\r\n");
            is_write_success = 1;
        }
        break;
    case KVS_CMD_VSEARCH:
        /* 高级语义向量相似度搜索：VSEARCH <二进制查询向量> [Top-K] */
        if (count < 2 || tokens[1].len % sizeof(float) != 0) {
            SET_RESP(response, "-ERR invalid query vector binary data\r\n");
        } else {
            int k_neighbors = (count > 2) ? fast_atoi(tokens[2].ptr) : 5; 
            
            /* 触发底层 FAISS ANN (近似最近邻) 召回引擎，直接在 C++ 层构造 RESP 写入网络输出 */
            length = kvs_vector_search((float*)tokens[1].ptr, k_neighbors, response, KVS_MAX_RSP_LEN);
        }
        break;
        
    case KVS_CMD_DBSIZE:
        {
            if (g_config.role == ROLE_MASTER) {
                /* 拆分 else if，双通道独立刷盘排空 */
                while (g_xdp_batch_len > 0) { xdp_flush_to_slave(); if (g_xdp_batch_len > 0) usleep(10); }
                while (g_slave_wlen > 0) { tcp_flush_to_slave(); if (g_slave_wlen > 0) usleep(10); }
            }
            long long total_keys = 0;

    #if ENABLE_ARRAY
        total_keys += kvs_array_count(&global_array);
    #endif
    #if ENABLE_RBTREE
        total_keys += kvs_rbtree_count(&global_rbtree);
    #endif
    #if ENABLE_HASH
        total_keys += kvs_hash_count(&global_hash);
    #endif
    #if ENABLE_SKIPLIST
        total_keys += kvs_skiplist_count(&global_skiplist);
    #endif

        length = snprintf(response, KVS_MAX_RSP_LEN, ":%lld\r\n", total_keys);
        return length;
    }
    default:
        SET_RESP(response, "-ERR unknown command\r\n");
        break;
    }

    /* --------------------------------------------------------------------------
     * 指令执行后置拦截器 (Post-Execution Write Hooks)
     * -------------------------------------------------------------------------- */
    if (is_write_success && g_is_loading == 0 && raw_cmd != NULL) {
        
        if (key != NULL) {
            /* 1. 将有效变更触发零拷贝落盘 (基于 io_uring 的异步投递) */
            if (g_config.persistence_mode == PERSIST_AOF) {
                kvs_aof_append(raw_cmd, raw_len);
            }
            
            /* 2. 主节点拦截：触发数据透传微批聚合逻辑 */
            if (g_config.role == ROLE_MASTER) {
                if (g_is_xdp_mode && raw_len <= 1350) {
                    xdp_forward_to_slave(raw_cmd, raw_len);
                } else {
                    tcp_forward_to_slave(raw_cmd, raw_len);
                }
            }
        }

        /* 3. 驱动引擎内部高频自动内存快照状态机 */
        #if AUTO_SAVE_THRESHOLD > 0
        static uint64_t write_counter = 0;
        write_counter++;
        
        if (unlikely(write_counter % AUTO_SAVE_THRESHOLD == 0)) {
            #if USE_BGSAVE_FOR_AUTO
                kvs_rdb_bgsave(); 
            #else
                kvs_rdb_save(); 
            #endif
        }
        #endif
    }
    
    return length;
}

/* ==========================================================================
 * 核心网络流控与协议解析调度层 (Stream Protocol & Micro-Batching Dispatcher)
 * ========================================================================== */

/**
 * @brief       核心流式协议状态机与业务总线
 * @param[in]   client_fd  触发网络事件的 TCP 描述符 (-1 为 AOF/RDB 本地重放)
 * @param[in]   msg        网络接收缓冲区的基址
 * @param[in]   length     网络接收缓冲区的物理长度
 * @param[out]  response   预分配的响应缓冲区
 * @param[out]  processed  传出实际消耗的有效流字节数 (用于处理 TCP 粘包/半包)
 * @return      int        写入响应缓冲区的总字节数，或返回加载的命令条数
 * @note        采用严格的游标步进机制处理 TCP 流的粘包与半包截断。
 * 内置了基于 Master 角色的全局 2ms 智能微批处理 (Smart Flush) 机制。
 */
extern void kvs_aof_flush_to_kernel(void);
int kvs_protocol(int client_fd, char *msg, int length, char *response, int *processed) {
    if (msg == NULL || length <= 0 || response == NULL) return -1;
    if (processed) *processed = 0;

    char *p_start = msg;             
    int remain_len = length;         
    int total_response_len = 0;      
    char *current_resp_ptr = response; 
    
    int loaded_cmd_count = 0; 

    kvs_token_t tokens[KVS_MAX_TOKENS]; 

    /* TCP 流式边界扫描与拆包循环 */
    while (remain_len > 0) {
        if (!g_keep_running) break; 
        tokens[0].ptr = NULL;

        int consumed = 0, count = 0;
        
        /* 协议嗅探：首字符为 '*' 判定为 RESP 协议，否则回退为纯文本行协议 */
        if (*p_start == '*') {
            consumed = kvs_parser_resp(p_start, remain_len, tokens);
            if (consumed > 0) {
                while (count < KVS_MAX_TOKENS && tokens[count].ptr != NULL) count++;
            }
        } else {
            char *line_end = memchr(p_start, '\n', remain_len);
            if (line_end) {
                int line_len = (line_end - p_start) + 1; 
                if (line_len > 1 && *(line_end - 1) == '\r') *(line_end - 1) = '\0'; 
                else *line_end = '\0'; 
                
                count = kvs_parser_inline(p_start, tokens);
                consumed = line_len; 
            } else {
                /* 发生 TCP 半包截断，中止解析等待后续报文拼接 */
                consumed = 0; 
            }
        }

        if (consumed > 0) {
            /* fd == -1 判定为系统初始化阶段的数据基线重放，越过所有网络发送限制 */
            if (client_fd == -1) {
                char throwaway[256];
                kvs_executor(client_fd, tokens, count, throwaway, p_start, consumed);
                loaded_cmd_count++; 
            } else {
                /* 容量安全：防范单次 Epoll 唤醒产生超过 1MB 的超大连续响应触发缓冲区溢出 */
                if (total_response_len > (1024 * 1024) - 1024) break; 
                
                int resp_len = kvs_executor(client_fd, tokens, count, current_resp_ptr, p_start, consumed);
                
                /* 复制链路控制面静默：丢弃对指定 FD 的响应报文 */
                if (client_fd >= 0 && client_fd < 65536 && g_muted_fds[client_fd]) {
                    resp_len = 0; 
                }

                current_resp_ptr += resp_len;
                total_response_len += resp_len;

                /* * 复制游标自增引擎：Slave 消费数据后自动推进全局 Offset。
                 * 需严格排除心跳探测、集群拓扑交互等无状态指令。
                 */
                if (g_config.role == ROLE_SLAVE && tokens[0].ptr != NULL) {
                    int is_ctrl_cmd = 0;
                    int tlen = tokens[0].len;
                    char *tcmd = tokens[0].ptr;
                    
                    if (tlen == 4 && strncasecmp(tcmd, "PING", 4) == 0) is_ctrl_cmd = 1;
                    else if (tlen == 4 && strncasecmp(tcmd, "INFO", 4) == 0) is_ctrl_cmd = 1;
                    else if (tlen == 5 && strncasecmp(tcmd, "PSYNC", 5) == 0) is_ctrl_cmd = 1;
                    else if (tlen == 9 && strncasecmp(tcmd, "REPL_MUTE", 9) == 0) is_ctrl_cmd = 1;
                    
                    if (!is_ctrl_cmd) {
                        g_slave_repl_offset += consumed; 
                    }
                }
            }
            
            p_start += consumed;
            remain_len -= consumed;
        } else if (consumed == 0) {
            break; 
        } else {
            return -1; 
        }
    }

    if (processed) *processed = length - remain_len;

    /* ==========================================================
     * 工业级智能微批处理与滞留排空 (Smart Flush & Drain)
     * ========================================================== */
    if (client_fd > 0) {
        /* TCP 事件驱动模式 (客户端交互)
         * 当前 recv() 收到的指令批次已全部解析执行完毕。
         * CPU 即将交还给 epoll_wait 挂起休眠。
         * 此时必须立刻【强制排空】所有内存缓冲，实现人机对话的“零延迟”！
         */
        // 1. 强制将滞留的内存指令刷入 AOF 磁盘文件
        if (g_config.persistence_mode == PERSIST_AOF) {
            kvs_aof_flush_to_kernel();
        }
        
        // 2. 强制将滞留的网络包发往从机
        if (g_config.role == ROLE_MASTER) {
            if (g_xdp_batch_len > 0) xdp_flush_to_slave();
            if (g_slave_wlen > 0) tcp_flush_to_slave();
        }
    } 
    else {
        /* 高频内部轮询模式 (XDP 底层收包 / AOF 历史重放)
         * 维持 2ms 延迟合并机制，保护磁盘与物理网卡不被单包系统调用风暴击穿
         */
        static struct timeval last_flush_time = {0};
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_us = (now.tv_sec - last_flush_time.tv_sec) * 1000000 + 
                          (now.tv_usec - last_flush_time.tv_usec);

        if (elapsed_us > 2000) {
            if (g_config.persistence_mode == PERSIST_AOF) {
                kvs_aof_flush_to_kernel();
            }
            if (g_config.role == ROLE_MASTER) {
                if (g_xdp_batch_len > 0) xdp_flush_to_slave();
                if (g_slave_wlen > 0) tcp_flush_to_slave();
            }
            last_flush_time = now;
        }
    }

    return (client_fd == -1) ? loaded_cmd_count : total_response_len;
}

/* ==========================================================================
 * 全局数据结构引擎生命周期 (Data Engine Lifecycle)
 * ========================================================================== */

int init_kvengine(void) {
#if ENABLE_ARRAY
    memset(&global_array, 0, sizeof(kvs_array_t));
    kvs_array_create(&global_array);
#endif
#if ENABLE_RBTREE
    memset(&global_rbtree, 0, sizeof(kvs_rbtree_t));
    kvs_rbtree_create(&global_rbtree);
#endif
#if ENABLE_HASH
    memset(&global_hash, 0, sizeof(kvs_hash_t));
    kvs_hash_create(&global_hash);
#endif
#if ENABLE_SKIPLIST
    memset(&global_skiplist, 0, sizeof(kvs_skiplist_t));
    kvs_skiplist_create(&global_skiplist);
#endif

    kvs_vector_init(1536);
    kvs_log(LOG_INFO, "[Engine] FAISS Vector Engine initialized (Dim: 1536).");
    return 0;
}

void dest_kvengine(void) {
#if ENABLE_ARRAY
    kvs_array_destory(&global_array);
#endif
#if ENABLE_RBTREE
    kvs_rbtree_destory(&global_rbtree);
#endif
#if ENABLE_HASH
    kvs_hash_destory(&global_hash);
#endif
#if ENABLE_SKIPLIST
    kvs_skiplist_destory(&global_skiplist);
#endif
}

/* ==========================================================================
 * 底层网络栈重置与 RDMA 直通准备 (OS Network Stack Configuration)
 * ========================================================================== */

void setup_network_environment() {
    int ret;
    printf("========================================\n");
    printf(" [Network] Initializing Environment...\n");
    printf("========================================\n");

    /* 强行剥离系统的网络管理器接管，防止配置被覆盖 */
    printf("[0/4] Detaching interface from NetworkManager...\n");
    ret = system("nmcli dev set eth_xdp managed no 2>/dev/null");

    /* 重置链路状态，刷新缓存地址并关闭硬件网卡的接收端哈希 (RxHash) 以集中引流 */
    printf("[1/4] Resetting interface and hardware queues...\n");
    ret = system("ip link set dev eth_xdp down");
    ret = system("ip link set dev eth_xdp up");
    ret = system("ip addr flush dev eth_xdp 2>/dev/null");
    
    ret = system("ethtool -L eth_xdp combined 1 2>/dev/null");
    ret = system("ethtool -K eth_xdp rxhash off 2>/dev/null");
    ret = system("ip link set dev eth_xdp promisc on");

    /* 配置静态路由防 ARP 泛洪，直接进行硬编码地址映射 */
    if (g_config.role == ROLE_MASTER) {
        printf("[2/4] Configuring MASTER IP and Static ARP...\n");
        ret = system("ip addr add 192.168.124.13/24 dev eth_xdp");
        ret = system("arp -s 192.168.124.14 00:0c:29:2c:80:22");
    } 
    else {
        printf("[2/4] Configuring SLAVE IP and Static ARP...\n");
        ret = system("ip addr add 192.168.124.14/24 dev eth_xdp");
        ret = system("arp -s 192.168.124.13 00:0c:29:de:bc:bf");
    }

    /* 基于物理网卡建立软件 RDMA 仿真设备层 (SoftRoCE) */
    printf("[3/4] Rebuilding RDMA Virtual Link (SoftRoCE)...\n");
    ret = system("rdma link delete rxe0 2>/dev/null");
    ret = system("rdma link add rxe0 type rxe netdev eth_xdp 2>/dev/null");

    printf("[4/4] Waiting for Kernel Network Stack to stabilize...\n");
    sleep(2); 
    
    if (g_config.role == ROLE_SLAVE) {
        printf("[Slave] Verifying physical link to Master (192.168.124.13)...\n");
        int max_retries = 10;
        int ping_success = 0;
        
        for (int i = 0; i < max_retries; i++) {
            if (system("ping -c 1 -W 1 192.168.124.13 > /dev/null 2>&1") == 0) {
                ping_success = 1;
                break;
            }
            printf("[Slave] Master unreachable. Retrying %d/%d...\n", i + 1, max_retries);
            sleep(1);
        }

        if (!ping_success) {
            printf("\n[Fatal] Master is DEAD or Network is unreachable!\n");
            exit(EXIT_FAILURE); 
        }
        printf("[Slave] Ping to Master SUCCESS! Physical link is perfectly clear.\n");
    }

    (void)ret; 
    printf("========================================\n");
}

/* ==========================================================================
 * XDP 共享内存极速消费引擎 (XDP SHM Polling Consumer)
 * ========================================================================== */

/**
 * @brief 提取毫秒级时间戳的内联原语
 */
static inline uint64_t get_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/**
 * @brief       XDP 旁路通道读队列消费协程
 * @note        采用非抢占式轮询调度。通过设立绝对时间片门槛 (2ms)，
 * 保证无锁死循环 (Busy-loop) 不会造成底层 TCP Reactor (Epoll) 发生线程级饥饿。
 */
void kvs_shm_rx_poll() {
    if (!g_shm || g_config.role != ROLE_SLAVE) return;

    char payload[MAX_PAYLOAD];
    uint32_t payload_len;
    
    uint64_t start_time = get_ms(); 
    int batch_count = 0;

    /* 无锁高频出队 */
    while (spsc_dequeue(&g_shm->rx_queue, payload, &payload_len)) {
        char throwaway_resp[1024]; 
        
        /* 直通协议解析器引擎，实现完全的内核协议栈绕过 */
        kvs_protocol(-1, payload, payload_len, throwaway_resp, NULL);
        
        batch_count++;
        
        /* 工业级时间片自适应：为防止阻塞主 I/O 事件循环，设置主动让出机制 */
        if ((batch_count % 100) == 0) {
            if (get_ms() - start_time >= 2) {
                break;
            }
        }
    }
}

/* ==========================================================================
 * 系统主引擎引导程序 (System Bootstrap Entry)
 * ========================================================================== */

int kvs_server_start(int argc, char *argv[]) {
    /* 特权级审计：XDP, eBPF 及 RDMA 均需要内核 CAP_NET_ADMIN 或 Root 权限 */
    if (getuid() != 0) {
        printf("\n[Fatal] Permission Denied!\n");
        printf("Please run the program as ROOT: sudo %s %s\n\n", argv[0], argc > 1 ? argv[1] : "");
        exit(EXIT_FAILURE);
    }

    /* 挂载黑匣子崩溃保护 */
    kvs_crash_guard_init();
    
    /* 挂载全局内存操作审计钩子 */
    mem_probe_init(1);

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN); 
    signal(SIGCHLD, sigchld_handler);

    kvs_config_init_default();
    const char *conf_file = (argc > 1 && strcmp(argv[1], "--probe") != 0) ? argv[1] : "kvstore.conf";
    kvs_config_load(conf_file);

    init_kvengine();
    
    if (g_config.persistence_mode != PERSIST_NONE) {
        kvs_persist_init();
    }

    setup_network_environment();

    /* 动态判定系统网卡绑定状态，激活对应的全双工零拷贝通信层 */
    g_is_xdp_mode = (g_config.ifname[0] != '\0' && strstr(g_config.ifname, "eth") != NULL);

    if (g_is_xdp_mode) {
        kvs_xdp_ipc_init(); 
    }

    kvs_log(LOG_INFO, "System starting pure KV Engine on port: %d", g_config.port);
    
    /* 角色鉴权：如果是 Slave，触发集群主从复制连接握手 */
    if (g_config.role == ROLE_SLAVE && strlen(g_config.master_ip) > 0) {
        kvs_log(LOG_INFO, "Connecting to Master %s:%d for Existing Data Sync (RDMA/TCP Target)...", 
                g_config.master_ip, g_config.master_port);
                
        int master_fd = kvs_slave_sync_with_master();
        
        /* 剥离握手用途后，固化为主从增量重传 (NACK/PSYNC) 的控制平面通道 */
        if (master_fd > 0) {
            extern int g_master_tcp_fd;
            g_master_tcp_fd = master_fd; 
            kvs_log(LOG_INFO, "TCP Control Channel to Master established (FD: %d).", g_master_tcp_fd);
        }
    }

    /* 架构倒置：将控制权交由指定的底层网络驱动框架 (Reactor / Proactor / NtyCo) */
    #if (NETWORK_SELECT == NETWORK_REACTOR)
        reactor_start(g_config.port, kvs_protocol, -1);
    #elif (NETWORK_SELECT == NETWORK_PROACTOR)
        proactor_start(g_config.port, kvs_protocol, -1);
    #elif (NETWORK_SELECT == NETWORK_NTYCO)
        ntyco_start(g_config.port, kvs_protocol);
    #endif

    /* ==========================================================
     * 引擎终结：保证进程退出的绝对安全落盘
     * ========================================================== */
    kvs_log(LOG_INFO, "Shutting down KV Engine safely...");

    if (g_config.persistence_mode == PERSIST_RDB) {
        kvs_log(LOG_INFO, "Graceful shutdown: Saving memory snapshot to RDB...");
        kvs_rdb_save(); 
        kvs_log(LOG_INFO, "RDB Snapshot saved successfully.");
    }

    if (g_config.role == ROLE_MASTER && g_slave_tcp_fd != -1) {
        close(g_slave_tcp_fd);
        g_slave_tcp_fd = -1;
    }

    /* 锁死 I/O 系统并释放 io_uring 描述符 */
    kvs_persist_stop(); 

    kvs_log(LOG_INFO, "Sweeping memory engines...");
    dest_kvengine(); 

    kvs_log(LOG_INFO, "All resources released. Goodbye!");
    return 0;
}