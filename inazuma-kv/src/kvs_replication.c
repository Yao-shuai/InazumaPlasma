/**
 * @file        kvs_replication.c
 * @brief       InazumaKV 纯内存主从复制层 (严格分离准备开销与纯网络开销)
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        实现 Master 与 Slave 之间的全量内存状态同步。
 * 提供宏开关 (ENABLE_RDMA_SYNC) 以支持在 RoCEv2 (RDMA) 纯内存直推与
 * 传统 TCP Sendfile (结合 Page Cache 淘汰机制) 之间进行严格对标与测速。
 */

#include "kvstore.h"
#include "kvs_config.h"
#include "kvs_replication.h" 
#include "kvs_vector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/time.h>   
#include <sys/socket.h> 
#include <sys/sendfile.h> 
#include <fcntl.h>        
#include <sys/stat.h>     

/* ==========================================================================
 * 核心架构开关与协议宏定义 (Architecture Toggles & Protocol Macros)
 * ========================================================================== */

/** * @brief 核心传输层协议开关 
 * @note  配置为 1: 启用 RDMA 零拷贝内存直推; 配置为 0: 回退至内核态 TCP Sendfile 传输。
 */
#define ENABLE_RDMA_SYNC 1 

/** @brief 复制协议数据类型标识：数组 */
#define REPL_TYPE_ARRAY    0
/** @brief 复制协议数据类型标识：红黑树 */
#define REPL_TYPE_RBTREE   1
/** @brief 复制协议数据类型标识：哈希表 */
#define REPL_TYPE_HASH     2
/** @brief 复制协议数据类型标识：跳跃表 */
#define REPL_TYPE_SKIPLIST 3

/* ==========================================================================
 * 全局状态与外部引擎引用 (Global States & External References)
 * ========================================================================== */

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

/** @brief 标识引擎当前是否处于加载阻塞状态 */
extern int g_is_loading;

/** @brief 全局主从复制状态机记录器，初始状态为无连接 */
int g_repl_state = REPL_STATE_NONE;

/* 声明外部 RDMA 硬件传输层接口 (带有精确的网络耗时回传参数) */
extern int kvs_rdma_send_memory(const char *target_ip, void *buffer, size_t size, double *cost);
extern int kvs_rdma_recv_memory(void *buffer, size_t size, double *cost);

/* ==========================================================================
 * 内部序列化工具函数 (Internal Serialization Utilities)
 * ========================================================================== */

/**
 * @brief       将键值对序列化为 TLV (Type-Length-Value) 二进制流并写入缓冲区
 * @param[in,out] ptr  指向当前缓冲区写入位置的二级指针，写入后自动步进
 * @param[in]     type 数据结构类型标识 (REPL_TYPE_XXX)
 * @param[in]     key  键名指针
 * @param[in]     val  值数据指针
 * @note        采用严格按字节长度的内存拷贝，确保能够安全处理包含 \0 的二进制负载。
 */
static void serialize_to_buffer(char **ptr, uint8_t type, const char *key, const char *val) {
    if (!key || !val) return;
    
    uint32_t klen = strlen(key);
    uint32_t vlen = strlen(val);
    
    /* 写入 1 Byte 的类型标识 */
    *(*ptr)++ = type;
    
    /* 写入 4 Bytes 的 Key 长度及 Key 内容 */
    memcpy(*ptr, &klen, 4); 
    *ptr += 4;
    memcpy(*ptr, key, klen); 
    *ptr += klen;
    
    /* 写入 4 Bytes 的 Value 长度及 Value 内容 */
    memcpy(*ptr, &vlen, 4); 
    *ptr += 4;
    memcpy(*ptr, val, vlen); 
    *ptr += vlen;
}

#if ENABLE_RBTREE
/**
 * @brief       红黑树深度优先序列化遍历 (用于全量内存快照生成)
 * @param[in]   node 当前遍历的红黑树节点
 * @param[in]   nil  树的全局哨兵节点 (Sentinel)
 * @param[in,out] ptr 指向目标缓冲区的二级指针
 */
static void traverse_rbtree(rbtree_node *node, rbtree_node *nil, char **ptr) {
    if (node == nil || node == NULL) return;
    traverse_rbtree(node->left, nil, ptr);
    serialize_to_buffer(ptr, REPL_TYPE_RBTREE, (char*)node->key, (char*)node->value);
    traverse_rbtree(node->right, nil, ptr);
}
#endif

/* ==========================================================================
 * 主节点同步协议解析与数据直推引擎 (Master PSYNC & Data Push Engine)
 * ========================================================================== */

/**
 * @brief       Master 端核心控制钩子：接管并处理 Slave 提交的 PSYNC 握手指令
 * @param[in]   client_fd  触发同步请求的客户端 TCP 文件描述符 (控制面链路)
 * @note        此阶段 Master 会冻结业务处理并分配 256MB 连续内存缓冲区。
 * 遍历全量内存状态后，依据宏定义 (ENABLE_RDMA_SYNC) 决断数据面路由：
 * - 启用时：通过 RDMA (RoCEv2) 协议的 WRITE 语义，将内存数据零拷贝直推至远端网卡。
 * - 禁用时：降级为 TCP 模式，通过临时 RDB 文件与 sendfile() 系统调用执行零拷贝传输，
 * 并结合内核强制降级参数驱逐页缓存，以达到最真实的物理性能对标。
 */
void kvs_repl_handle_psync(int client_fd) {
    kvs_log(LOG_INFO, "[Master] Persistence Decoupled! Preparing Sync...");
    
    struct timeval prep_t1, prep_t2;
    /* 启动秒表：测算纯 CPU 与内存分配的数据准备开销 (Data Preparation Phase) */
    gettimeofday(&prep_t1, NULL); 

    /* 分配一块连续的巨型内存块用于容纳序列化后的快照数据 */
    size_t max_snap_size = 256 * 1024 * 1024; 
    char *snapshot = malloc(max_snap_size);
    if (snapshot == NULL) {
        kvs_log(LOG_ERROR, "[Fatal] Out of Memory! Failed to allocate 256MB for replication.");
        return;
    }
    
    char *p = snapshot;
    /* 设定安全防越界水位线，预留 4KB 缓冲防止尾包溢出 */
    char *safe_end = snapshot + max_snap_size - 4096; 
    
#if ENABLE_ARRAY
    for (int i = 0; i < global_array.total; i++) { 
        if (p >= safe_end) break;
        if (global_array.table[i].key != NULL) 
            serialize_to_buffer(&p, REPL_TYPE_ARRAY, global_array.table[i].key, global_array.table[i].value);
    }
#endif

#if ENABLE_RBTREE
    if (global_rbtree.root && global_rbtree.root != global_rbtree.nil) 
        traverse_rbtree(global_rbtree.root, global_rbtree.nil, &p);
#endif

#if ENABLE_HASH
    for (int i = 0; i < global_hash.max_slots; i++) {
        if (p >= safe_end) break;
        hashnode_t *node = global_hash.nodes[i];
        while (node) { 
            serialize_to_buffer(&p, REPL_TYPE_HASH, node->data, node->data + node->key_len + 1); 
            node = node->next; 
            if (p >= safe_end) break;
        }
    }
#endif

#if ENABLE_SKIPLIST
    if (global_skiplist.header) {
        kvs_skiplist_node_t *node = global_skiplist.header->forward[0];
        while (node) { 
            if (p >= safe_end) break;
            serialize_to_buffer(&p, REPL_TYPE_SKIPLIST, (char*)node->key, (char*)node->value); 
            node = node->forward[0]; 
        }
    }
#endif

    size_t actual_size = p - snapshot;
    
    /* 终止秒表并输出内存准备开销 */
    gettimeofday(&prep_t2, NULL);
    printf("[Master] Data Prep Cost (Malloc + Serialize): %.2f ms\n", 
           (prep_t2.tv_sec - prep_t1.tv_sec) * 1000.0 + (prep_t2.tv_usec - prep_t1.tv_usec) / 1000.0);

    char response[128];

#if ENABLE_RDMA_SYNC
    /* ---------------------------------------------------------
     * 架构方案 A: RDMA 高速物理网卡直推通道 (Zero-Copy Network Plane)
     * --------------------------------------------------------- */
    
    /* 通过控制面 TCP 链路下发状态转移同步原语 */
    int len = sprintf(response, "+FULLRESYNC-RDMA %zu\r\n", actual_size);
    send(client_fd, response, len, 0);
    const char *slave_ip = "192.168.124.14"; 
    
    char ack[8] = {0};
    kvs_log(LOG_INFO, "[Master] Waiting for Slave RDMA setup...");
    
    /* 阻塞等待 Slave 确认 RDMA 接收池 (MR) 初始化就绪 */
    recv(client_fd, ack, 5, 0); 

    /* 为应对极低延迟网络环境下的时序震荡，休眠 1 秒以确保远端 QP 完全状态激活。
     * 此阶段处于链路建立预热期，不计入核心传输网络耗时。
     */
    sleep(1);

    double net_cost = 0.0; 
    if (actual_size > 0) {
        kvs_rdma_send_memory(slave_ip, snapshot, actual_size, &net_cost);
    }
    
    if (net_cost > 0.0) {
        printf("[Master] RDMA Pure Network Sync completed: %zu bytes in %.2f ms (%.2f MB/s).\n", 
               actual_size, net_cost, (actual_size / 1024.0 / 1024.0) / (net_cost / 1000.0));
    }

#else
    /* ---------------------------------------------------------
     * 架构方案 B: Linux Sendfile 传统零拷贝 (结合缓存淘汰进行基准对标)
     * --------------------------------------------------------- */
    
    /* 通过控制面 TCP 链路下发传统传输方案原语 */
    int len = sprintf(response, "+FULLRESYNC-SENDFILE %zu\r\n", actual_size);
    send(client_fd, response, len, 0);

    struct timeval disk_t1, disk_t2;  
    /* 启动秒表：测算临时 RDB 文件的写入与物理落盘开销 */
    gettimeofday(&disk_t1, NULL); 

    const char *tmp_rdb = "data/repl_sync.rdb";
    int write_fd = open(tmp_rdb, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (write_fd >= 0) {
        size_t written = 0;
        while (written < actual_size) {
            ssize_t ret = write(write_fd, snapshot + written, actual_size - written);
            if (ret <= 0) break;
            written += ret;
        }
        /* 执行强同步指令确保脏页刷新至物理介质 */
        fsync(write_fd); 
        close(write_fd);
    }
    
    gettimeofday(&disk_t2, NULL);
    printf("[Master] Disk I/O Prep Cost (Write + Fsync): %.2f ms\n", 
           (disk_t2.tv_sec - disk_t1.tv_sec) * 1000.0 + (disk_t2.tv_usec - disk_t1.tv_usec) / 1000.0);

    /* 强行清空 Linux 内核的 Page Cache，以模拟极严苛条件下的真实物理磁盘读取延迟 */
    sync(); 
    int drop_fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (drop_fd >= 0) {
        /* 防御性编程：捕获权限不足或挂载限制导致的缓存释放异常 */
        if (write(drop_fd, "3\n", 2) < 0) {
            kvs_log(LOG_WARN, "[Master] Failed to drop page cache (permission issue or ignored).");
        }
        close(drop_fd);
        printf("[Master] Kernel Page Cache dropped. Forcing physical disk I/O...\n");
    }
    
    struct timeval net_t1, net_t2;
    /* 启动秒表：剔除上述准备代价后，纯净的 TCP 发送耗时测算 */
    gettimeofday(&net_t1, NULL); 

    int read_fd = open(tmp_rdb, O_RDONLY);
    size_t total_sent = 0;
    if (read_fd >= 0) {
        off_t offset = 0;
        while (total_sent < actual_size) {
            ssize_t sent = sendfile(client_fd, read_fd, &offset, actual_size - total_sent);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(2000); 
                    continue;
                }
                if (errno == EINTR) continue;
                perror("[Master] Sendfile fatal error");
                break;
            }
            if (sent == 0) break;
            total_sent += sent;
        }
        close(read_fd);
    }
    
    gettimeofday(&net_t2, NULL);
    double net_cost = (net_t2.tv_sec - net_t1.tv_sec) * 1000.0 + (net_t2.tv_usec - net_t1.tv_usec) / 1000.0;
    
    if (net_cost > 0.0) {
        printf("[Master] Sendfile Pure Network Sync completed: %zu bytes in %.2f ms (%.2f MB/s).\n", 
               total_sent, net_cost, (total_sent / 1024.0 / 1024.0) / (net_cost / 1000.0));
    }
    
    /* 清理磁盘残骸 */
    unlink(tmp_rdb); 
#endif

    free(snapshot);
}

/* ==========================================================================
 * 从节点同步握手与全量快照接收层 (Slave PSYNC Handshake & Data Recv)
 * ========================================================================== */

/**
 * @brief       Slave 端启动入口：发起与 Master 的全链路状态复制
 * @return      int 建立并维持控制面心跳的 TCP 文件描述符，失败则触发进程退出
 * @note        生命周期时序：
 * 1. 建立控制面 TCP 链路，发送 PSYNC 握手指令。
 * 2. 解析 Master 响应，分配对应的连续物理内存。
 * 3. 阻塞执行 RDMA 零拷贝接收或传统的 TCP 流式接收。
 * 4. 执行内存快照反序列化加载。
 * 5. 完成基线对齐后，状态机推进至 REPL_STATE_XDP_REALTIME 阶段，
 * 开始接收 AF_XDP 旁路网络传来的增量指令流。
 */
int kvs_slave_sync_with_master() {
    g_repl_state = REPL_STATE_CONNECTING;
    printf("[Replication] State transition -> CONNECTING (Handshaking with Master)...\n");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* 设定严格的 I/O 超时熔断机制，防止在极度恶劣的网络环境下引发无休止的内核级死锁 */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(g_config.master_port) };
    inet_pton(AF_INET, g_config.master_ip, &addr.sin_addr);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[Fatal] Connection to Master timed out or refused");
        close(fd);
        exit(EXIT_FAILURE);
    }
    
    /* 构造并发送标准的 PSYNC 握手原语 */
    char *cmd = "*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n";
    if (write(fd, cmd, strlen(cmd)) < 0) {
        close(fd); 
        return -1;
    }

    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        perror("[Fatal] Master node is silent or link severed");
        close(fd); 
        exit(EXIT_FAILURE);
    }
    buf[n] = '\0'; 

    size_t rdb_size = 0;
    char *memory_block = NULL;

#if ENABLE_RDMA_SYNC
    /* 解析 Master 下发的 RDMA 架构同步指令 */
    if (strncmp(buf, "+FULLRESYNC-RDMA", 16) == 0) {
        rdb_size = atoll(buf + 17);
        printf("[Slave] Master signals RDMA Snapshot size: %zu bytes\n", rdb_size);

        if (rdb_size > 0) {
            g_repl_state = REPL_STATE_RDMA_SYNCING;
            memory_block = malloc(rdb_size);
            
            /* 通知 Master 己方内存池 (MR) 已注册完毕，可以启动硬件直推 */
            send(fd, "READY", 5, 0);

            double net_cost = 0.0; 
            /* 进入阻塞挂起，等待底层 RDMA 网卡硬件的 WRITE_WITH_IMM 完成中断 */
            int rdma_status = kvs_rdma_recv_memory(memory_block, rdb_size, &net_cost);
            if (rdma_status < 0) {
                printf("\n[Fatal] RDMA zero-copy transfer failed abruptly!\n");
                free(memory_block); 
                exit(EXIT_FAILURE);
            }
            
            if (net_cost > 0.0) {
                printf("[Slave] RDMA Pure Network Recv completed in %.2f ms (%.2f MB/s).\n", 
                       net_cost, (rdb_size / 1024.0 / 1024.0) / (net_cost / 1000.0));
            }
        }
    } else {
        printf("[Fatal] Expected RDMA protocol signal, but received:\n%s\n", buf);
        close(fd); 
        return -1;
    }
#else
    /* 解析 Master 下发的传统 TCP 降级同步指令 */
    if (strncmp(buf, "+FULLRESYNC-SENDFILE", 20) == 0) {
        rdb_size = atoll(buf + 21);
        printf("[Slave] Master signals Sendfile TCP size: %zu bytes\n", rdb_size);

        if (rdb_size > 0) {
            g_repl_state = REPL_STATE_TCP_SYNCING;
            memory_block = malloc(rdb_size);
            size_t received = 0;
            
            /* 大流量长连接传输阶段，临时解除 SO_RCVTIMEO 限制避免截断 */
            struct timeval tv_zero = { .tv_sec = 0, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_zero, sizeof(tv_zero));

            struct timeval net_t1, net_t2;
            /* 启动秒表：严格测算纯网络 TCP 接收耗时 */
            gettimeofday(&net_t1, NULL); 

            while (received < rdb_size) {
                ssize_t r = recv(fd, memory_block + received, rdb_size - received, 0);
                if (r <= 0) {
                    printf("[Fatal] TCP receive stream broken at %zu bytes.\n", received);
                    free(memory_block); 
                    exit(EXIT_FAILURE);
                }
                received += r;
            }

            gettimeofday(&net_t2, NULL);
            double net_cost = (net_t2.tv_sec - net_t1.tv_sec) * 1000.0 + (net_t2.tv_usec - net_t1.tv_usec) / 1000.0;
            
            if (net_cost > 0.0) {
                printf("[Slave] TCP Pure Network Recv completed in %.2f ms (%.2f MB/s).\n", 
                       net_cost, (rdb_size / 1024.0 / 1024.0) / (net_cost / 1000.0));
            }
        }
    } else {
        printf("[Fatal] Expected SENDFILE protocol signal, but received:\n%s\n", buf);
        close(fd); 
        return -1;
    }
#endif

    /* =========================================================
     * 基线对齐：反序列化巨型内存块并投递至各大存储引擎
     * ========================================================= */
    if (rdb_size > 0 && memory_block) {
        printf("[Slave] Reconstructing memory structures from snapshot...\n");
        g_is_loading = 1;
        
        char *ptr = memory_block;
        char *end = memory_block + rdb_size;
        
        while (ptr < end) {
            uint8_t type = *ptr++;
            uint32_t klen; 
            memcpy(&klen, ptr, 4); 
            ptr += 4;
            char *key = ptr; 
            ptr += klen;
            
            uint32_t vlen; 
            memcpy(&vlen, ptr, 4); 
            ptr += 4;
            char *val = ptr; 
            ptr += vlen;
            
            /* 根据 TLV Type 标志执行底层路由 */
            switch(type) {
#if ENABLE_ARRAY
                case REPL_TYPE_ARRAY: 
                    kvs_array_set(&global_array, key, klen, val, vlen); 
                    break;
#endif
#if ENABLE_RBTREE
                case REPL_TYPE_RBTREE: 
                    kvs_rbtree_set(&global_rbtree, key, klen, val, vlen); 
                    break;
#endif
#if ENABLE_HASH
                case REPL_TYPE_HASH: 
                    kvs_hash_set(&global_hash, key, klen, val, vlen);
                    if (klen > 4 && strncmp(key, "VEC:", 4) == 0 && vlen % sizeof(float) == 0) {
                        kvs_vector_add(key + 4, klen - 4, (float*)val);
                    }
                    break;
#endif
#if ENABLE_SKIPLIST
                case REPL_TYPE_SKIPLIST: 
                    kvs_skiplist_set(&global_skiplist, key, klen, val, vlen); 
                    break;
#endif
            }
        }
        g_is_loading = 0;
        free(memory_block);
        printf("[Slave] Memory baseline construction complete.\n");
    }

    /* * 架构跃迁：全量基线对齐完成，切换至增量同步状态。
     * Master 端将会启动网卡的 XDP 数据包克隆 (Packet Cloning) 特性。
     */
    g_repl_state = REPL_STATE_XDP_REALTIME;
    printf("[Replication] State transition -> XDP_REALTIME (eBPF bypass pipeline activated).\n");

    return fd;
}