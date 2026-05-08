/**
 * @file        kvstore.h
 * @brief       InazumaKV 核心存储引擎与系统架构总控头文件
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        统领全局的数据结构引擎宏开关、网络模型规约、内存管理原语
 * 以及协议解析与执行层的公共接口定义。
 */

#ifndef __KV_STORE_H__
#define __KV_STORE_H__

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>

/* 系统级与网络 I/O 依赖 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sendfile.h> 
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ==========================================================================
 * 全局架构与网络模型规约 (Global Architecture & Network Models)
 * ========================================================================== */

/** @brief 开启批量 (Pipeline) 协议解析模式 (1: 开启, 0: 单指令模式) */
#define ENABLE_BATCH_PROTOCOL   1

/* 网络驱动模型枚举 */
#define NETWORK_REACTOR     0   /**< Epoll 单线程异步事件驱动模型 */
#define NETWORK_PROACTOR    1   /**< io_uring 纯异步 I/O 驱动模型 */
#define NETWORK_NTYCO       2   /**< NtyCo 用户态协程驱动模型 */

/* ==========================================================================
 * 缓冲区与协议限制 (Buffer & Protocol Constraints)
 * ========================================================================== */

#define KVS_MAX_TOKENS      128          /**< 单条 RESP 指令最大解析 Token 数 */
#define KVS_MAX_MSG_LEN     (128 * 1024) /**< 接收缓冲区的最大容量边界 (128KB) */
#define KVS_MAX_RSP_LEN     (128 * 1024) /**< 响应缓冲区的最大容量边界 (128KB) */

/* ==========================================================================
 * 数据结构引擎特性开关 (Engine Feature Toggles)
 * ========================================================================== */

#define ENABLE_ARRAY        1   /**< 开启基于数组的线性存储引擎 */
#define ENABLE_RBTREE       1   /**< 开启基于红黑树的有序存储引擎 */
#define ENABLE_HASH         1   /**< 开启基于哈希表的 $O(1)$ 存储引擎 */
#define ENABLE_SKIPLIST     1   /**< 开启基于跳跃表的概率型有序存储引擎 */

/* ==========================================================================
 * 全局运行状态与回调定义 (Global States & Callbacks)
 * ========================================================================== */

/** @brief 标识系统是否处于 AOF/RDB 的启动加载阶段 */
extern int g_is_loading;

/** * @brief 核心协议处理钩子定义 
 * @param[in] client_fd 发起请求的客户端文件描述符
 * @param[in] msg 接收到的原始报文
 * @param[in] length 原始报文长度
 * @param[out] response 承接响应数据的缓冲区
 * @param[out] processed 用于传出实际成功解析并执行的字节数
 * @return 成功解析的命令数量，负数表示协议错误
 */
typedef int (*msg_handler)(int client_fd, char *msg, int length, char *response, int *processed);

/* 网络层启动入口声明 */
extern int reactor_start(unsigned short port, msg_handler handler, int master_fd);
extern int proactor_start(unsigned short port, msg_handler handler, int master_fd);
extern int ntyco_start(unsigned short port, msg_handler handler);


/* ==========================================================================
 * 线性数组存储引擎 (Array Engine)
 * ========================================================================== */
#if ENABLE_ARRAY

typedef struct {
    char *key;
    int key_len;   /**< 严格保存 Key 的真实物理长度以保障二进制安全 */
    char *value;
    int val_len;   /**< 严格保存 Value 的真实物理长度以保障二进制安全 */
} kvs_array_item_t;

#define KVS_ARRAY_SIZE      1048576

typedef struct kvs_array_s {
    kvs_array_item_t *table;
    int idx;
    int total;
    int count;
} kvs_array_t;

int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, char *key, int key_len, char *value, int val_len);
char* kvs_array_get(kvs_array_t *inst, char *key, int key_len, int *out_vlen);
int kvs_array_del(kvs_array_t *inst, char *key, int key_len);
int kvs_array_mod(kvs_array_t *inst, char *key, int key_len, char *value, int val_len);
int kvs_array_exist(kvs_array_t *inst, char *key, int key_len);
int kvs_array_count(kvs_array_t *inst);
#endif

/* ==========================================================================
 * 红黑树存储引擎 (Red-Black Tree Engine)
 * ========================================================================== */
#if ENABLE_RBTREE

#define RED             1
#define BLACK           2

#define ENABLE_KEY_CHAR     1

#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE; 
#endif

typedef struct _rbtree_node {
    unsigned char color;
    struct _rbtree_node *right;
    struct _rbtree_node *left;
    struct _rbtree_node *parent;
    void *key;
    int key_len;    
    void *value;
    int val_len;    
} rbtree_node;

typedef struct _rbtree {
    rbtree_node *root;
    rbtree_node *nil;
    int count;
} rbtree;

typedef struct _rbtree kvs_rbtree_t;

int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst);
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, int key_len, char *value, int val_len);
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key, int key_len, int *out_vlen);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key, int key_len);
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, int key_len, char *value, int val_len);
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key, int key_len);
int kvs_rbtree_count(kvs_rbtree_t *inst);

#endif

/* ==========================================================================
 * 哈希表存储引擎 (Hash Table Engine)
 * ========================================================================== */
#if ENABLE_HASH

#define MAX_KEY_LEN     128
#define MAX_VALUE_LEN   512
#define MAX_TABLE_SIZE  8388608

#define ENABLE_KEY_POINTER  1

typedef struct hashnode_s {
    struct hashnode_s *next;
    int key_len;       
    int val_len;      
    /** * @note C99 柔性数组 (Flexible Array Member)。
     * 此字段不占用结构体的固定 `sizeof` 空间，配合动态内存分配实现
     * Key 和 Value 数据与节点元信息的内存连续排列，极大优化 L1/L2 缓存命中率。
     */
    char data[]; 
} hashnode_t;

typedef struct hashtable_s {
    hashnode_t **nodes; 
    int max_slots;
    int count;
} hashtable_t;

typedef struct hashtable_s kvs_hash_t;

int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(kvs_hash_t *hash, char *key, int key_len, char *value, int val_len);
char *kvs_hash_get(kvs_hash_t *hash, char *key, int key_len, int *out_vlen);
int kvs_hash_del(kvs_hash_t *hash, char *key, int key_len);
int kvs_hash_mod(kvs_hash_t *hash, char *key, int key_len, char *value, int val_len);
int kvs_hash_exist(kvs_hash_t *hash, char *key, int key_len);
int kvs_hash_count(kvs_hash_t *hash);

#endif

/* ==========================================================================
 * 跳跃表存储引擎 (Skip List Engine)
 * ========================================================================== */
#if ENABLE_SKIPLIST

#define KVS_SKIPLIST_MAXLEVEL 32

typedef struct kvs_skiplist_node_s {
    char *key;
    int key_len;           
    char *value;
    int val_len;           
    struct kvs_skiplist_node_s **forward;
} kvs_skiplist_node_t;

typedef struct kvs_skiplist_s {
    int level;                      /**< 当前已分配的最大有效层级 */
    kvs_skiplist_node_t *header;    /**< 哨兵头节点 */
    int count;                      /**< 当前承载的节点总数 */
} kvs_skiplist_t;

int kvs_skiplist_create(kvs_skiplist_t *inst);
void kvs_skiplist_destory(kvs_skiplist_t *inst);
int kvs_skiplist_set(kvs_skiplist_t *inst, char *key, int key_len, char *value, int val_len);
char* kvs_skiplist_get(kvs_skiplist_t *inst, char *key, int key_len, int *out_vlen);
int kvs_skiplist_del(kvs_skiplist_t *inst, char *key, int key_len);
int kvs_skiplist_mod(kvs_skiplist_t *inst, char *key, int key_len, char *value, int val_len);
int kvs_skiplist_exist(kvs_skiplist_t *inst, char *key, int key_len);
int kvs_skiplist_count(kvs_skiplist_t *inst); 

#endif

/* ==========================================================================
 * 内存分配器接口 (Memory Allocator Abstractions)
 * ========================================================================== */
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);

/* ==========================================================================
 * 协议解析与引擎执行层 (Protocol Parsing & Execution)
 * ========================================================================== */

/**
 * @brief 零拷贝解析分词单元
 */
typedef struct {
    char *ptr; /**< 指向网络接收缓冲区内部的起始地址 */
    int len;   /**< 分词对应的真实物理长度 */
} kvs_token_t;

int kvs_parser_resp(char *msg, int length, kvs_token_t *tokens);
int kvs_parser_inline(char *msg, kvs_token_t *tokens);
int kvs_parser(char *msg, kvs_token_t *tokens);
int kvs_executor(int client_fd, kvs_token_t *tokens, int count, char *response, const char *raw_cmd, int raw_len);
int kvs_protocol(int client_fd, char *msg, int length, char *response, int *processed);

/* ==========================================================================
 * 内存操作防御性内联函数 (Defensive Memory Utilities)
 * ========================================================================== */

/**
 * @brief       兼容 jemalloc 的字符串安全深拷贝
 * @param[in]   s 标准 C 风格字符串
 * @return      char* 拷贝产生的新内存基址
 * @note        确保 Key/Value 数据彻底脱离网络接收缓冲区独立存在，
 * 防止连接关闭或缓冲区复用导致的数据篡改。
 */
static inline char *kvs_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *copy = (char *)kvs_malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

/**
 * @brief       执行严格的二进制安全内存深拷贝
 * @param[in]   s    源数据块基址
 * @param[in]   len  源数据块的绝对物理长度
 * @return      char* 拷贝分配的新内存基址
 * @note        基于长度边界进行拷贝，无视二进制数据流中可能截断的 '\0' 字节。
 * 并在末尾额外填充单字节零作为安全兜底防范。
 */
static inline char *kvs_memdup(const char *s, size_t len) {
    if (!s || len <= 0) return NULL;
    char *copy = (char *)kvs_malloc(len + 1); 
    if (copy) {
        memcpy(copy, s, len);   
        copy[len] = '\0';       
    }
    return copy;
}

#endif // __KV_STORE_H__