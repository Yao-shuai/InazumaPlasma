/**
 * @file        kvs_hash.c
 * @brief       InazumaKV 哈希存储引擎实现
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note       
 * 1. 彻底拥抱 jemalloc，利用外部优秀的内存分配器处理并发与碎片。
 * 2. 连续内存布局 (Node + Key + Value 一次分配)，最大化利用 CPU Cache Line。
 * 3. 极速内存比对与混合哈希算法抗碰撞。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "kvstore.h"

/* ==========================================================================
 * 宏定义与底层编译优化 (Compiler Optimizations)
 * ========================================================================== */

#ifndef unlikely
/** @brief 分支预测优化：告诉编译器条件大概率为假，优化指令流水线 */
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif


/* ==========================================================================
 * 全局实例 (Global Instances)
 * ========================================================================== */

/** @brief 全局哈希表单例实例 */
kvs_hash_t global_hash = {0};


/* ==========================================================================
 * 底层高频工具集 (High-Frequency Internal Helpers)
 * ========================================================================== */

/**
 * @brief       寄存器级 8 字节长步长比对
 * @param[in]   p1   参与比较的内存首地址1
 * @param[in]   p2   参与比较的内存首地址2
 * @param[in]   len  需要比较的字节长度
 * @return      int  0: 完全相等; 1: 不相等
 * @note        利用 uint64_t 和 uint32_t 强制转换，单条指令比对 8/4 个字节，
 * 大幅超越传统 for 循环逐字节比对的效率。
 */
static inline int fast_cmp(const char *p1, const char *p2, int len) {
    int i = 0;
    // 每次跨越 8 字节
    while (len - i >= 8) {
        if (*(uint64_t*)(p1 + i) != *(uint64_t*)(p2 + i)) return 1;
        i += 8;
    }
    // 处理剩余 4 字节块
    if (len - i >= 4) {
        if (*(uint32_t*)(p1 + i) != *(uint32_t*)(p2 + i)) return 1;
        i += 4;
    }
    // 尾部逐字节对齐处理
    while (i < len) {
        if (p1[i] != p2[i]) return 1;
        i++;
    }
    return 0;
}

/**
 * @brief       FNV-1a + Murmur3 混合位移散列
 * @param[in]   key      键的二进制数据
 * @param[in]   key_len  键的长度
 * @param[in]   size     哈希表槽位总数 (需为 2 的幂次)
 * @return      int      计算出的哈希槽索引
 */
static inline int _hash(char *key, int key_len, int size) {
    if (!key || key_len <= 0) return 0;
    
    unsigned int h = 2166136261u; // FNV_offset_basis
    
    // FNV-1a core
    for (int i = 0; i < key_len; i++) {
        h ^= (unsigned char)key[i];
        h *= 16777619; // FNV_prime
    }
    
    // Murmur3 mix/avalanche
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    
    // 取模优化：使用位与运算取代昂贵的取模 (%)，要求 size 必须是 2 的幂次
    return h & (size - 1); 
}

/** @brief 内联：根据连续内存布局，计算 Key 的物理首地址 */
static inline char* get_key(hashnode_t *node) { return node->data; }

/** @brief 内联：根据连续内存布局，计算 Value 的物理首地址 (跳过 Key 和 \0) */
static inline char* get_val(hashnode_t *node) { return node->data + node->key_len + 1; }

/**
 * @brief       一次性连续分配 (Node + Key + Value)
 * @param[in]   key      键的二进制数据
 * @param[in]   key_len  键的长度
 * @param[in]   value    值的二进制数据
 * @param[in]   val_len  值的长度
 * @return      hashnode_t* 新分配并组装完毕的节点指针；失败返回 NULL
 * @note        此设计极其巧妙！规避了多次 malloc 造成的内存碎片，
 * 配合 jemalloc 食用，极大提升并发分配效率和 CPU Cache 命中率。
 */
static hashnode_t *_create_node(char *key, int key_len, char *value, int val_len) {
    size_t total_size = sizeof(hashnode_t) + key_len + 1 + val_len + 1;
    
    // 彻底拥抱底层的 jemalloc，享受无锁并发与动态缩容
    hashnode_t *node = (hashnode_t*)kvs_malloc(total_size);
    if (!node) return NULL;
    
    node->key_len = key_len;
    node->val_len = val_len;
    node->next = NULL;

    char *k_ptr = get_key(node);
    char *v_ptr = get_val(node);
    
    // 内存拷贝并自动封口 (\0)，保障 C 风格字符串兼容性
    memcpy(k_ptr, key, key_len);
    k_ptr[key_len] = '\0';
    memcpy(v_ptr, value, val_len);
    v_ptr[val_len] = '\0';

    return node;
}


/* ==========================================================================
 * 对外暴露接口 (Public KV APIs)
 * ========================================================================== */

/**
 * @brief       初始化哈希表实例
 * @param[in]   hash  待初始化的哈希表指针
 * @return      int   0: 成功; -1: 参数错误或内存分配失败
 */
int kvs_hash_create(kvs_hash_t *hash) {
    if (!hash) return -1;
    
    hash->max_slots = MAX_TABLE_SIZE;
    hash->nodes = (hashnode_t**)kvs_malloc(sizeof(hashnode_t*) * hash->max_slots);
    
    if (!hash->nodes) return -1;
    
    // 槽位指针全量清零，防止野指针
    memset(hash->nodes, 0, sizeof(hashnode_t*) * hash->max_slots);
    hash->count = 0; 
    
    return 0;
}

/**
 * @brief       销毁哈希表实例并释放所有链表节点
 * @param[in]   hash  待销毁的哈希表指针
 * @note        函数名拼写遗留 (destory) 保持不变，兼容外部调用。
 */
void kvs_hash_destory(kvs_hash_t *hash) {
    if (!hash || !hash->nodes) return;
    
    for (int i = 0; i < hash->max_slots; i++) {
        hashnode_t *node = hash->nodes[i];
        while (node != NULL) {
            hashnode_t *tmp = node;
            node = node->next;
            kvs_free(tmp); // 优雅归还给 jemalloc，消除囤积
        }
    }
    
    kvs_free(hash->nodes);
    hash->nodes = NULL;
    hash->count = 0;
}

/**
 * @brief       插入 Key-Value (拉链法解决冲突)
 * @param[in]   hash     哈希表指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @param[in]   value    值数据
 * @param[in]   val_len  值长度
 * @return      int      0: 成功; 1: Key已存在; -1: 参数错误; -2: OOM
 */
int kvs_hash_set(kvs_hash_t *hash, char *key, int key_len, char *value, int val_len) {
    if (!hash || !key || !value) return -1;
    
    int idx = _hash(key, key_len, hash->max_slots);
    hashnode_t *node = hash->nodes[idx];

    // 冲突链表遍历查重
    while (node != NULL) {
        if (node->key_len == key_len && fast_cmp(get_key(node), key, key_len) == 0) return 1;
        node = node->next;
    }

    hashnode_t *new_node = _create_node(key, key_len, value, val_len);
    if (!new_node) return -2;

    // 头插法接入链表
    new_node->next = hash->nodes[idx];
    hash->nodes[idx] = new_node;
    hash->count++;
    
    return 0;
}

/**
 * @brief       获取指定 Key 的 Value
 * @param[in]   hash     哈希表指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @param[out]  out_vlen 用于传出值长度的指针
 * @return      char* 查找到的 Value 首地址；未找到返回 NULL
 */
char *kvs_hash_get(kvs_hash_t *hash, char *key, int key_len, int *out_vlen) {
    if (!hash || !key) return NULL;
    
    int idx = _hash(key, key_len, hash->max_slots);
    hashnode_t *node = hash->nodes[idx];

    while (node != NULL) {
        if (node->key_len == key_len && fast_cmp(get_key(node), key, key_len) == 0) {
            if (out_vlen) *out_vlen = node->val_len; 
            return get_val(node);
        }
        node = node->next;
    }
    
    return NULL;
}

/**
 * @brief       删除指定的 Key-Value
 * @param[in]   hash     哈希表指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @return      int      0: 删除成功; 1: Key不存在; -2: 参数错误
 */
int kvs_hash_del(kvs_hash_t *hash, char *key, int key_len) {
    if (!hash || !key) return -2;
    
    int idx = _hash(key, key_len, hash->max_slots);
    hashnode_t *head = hash->nodes[idx];
    
    if (head == NULL) return 1; 

    // Case 1: 删除的节点是头节点
    if (head->key_len == key_len && fast_cmp(get_key(head), key, key_len) == 0) {
        hash->nodes[idx] = head->next;
        kvs_free(head); // 直接交还 jemalloc
        hash->count--;
        return 0;
    }

    // Case 2: 删除的节点在链表中段或尾部
    hashnode_t *cur = head;
    while (cur->next != NULL) {
        if (cur->next->key_len == key_len && fast_cmp(get_key(cur->next), key, key_len) == 0) {
            hashnode_t *tmp = cur->next;
            cur->next = tmp->next;
            kvs_free(tmp); // 直接交还 jemalloc
            hash->count--;
            return 0;
        }
        cur = cur->next;
    }
    
    return 1; 
}

/**
 * @brief       修改指定 Key 的 Value
 * @param[in]   hash     哈希表指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @param[in]   value    新的值数据
 * @param[in]   val_len  新的值长度
 * @return      int      0: 成功; 1: Key不存在; -1: 参数错误
 */
int kvs_hash_mod(kvs_hash_t *hash, char *key, int key_len, char *value, int val_len) {
    if (!hash || !key || !value) return -1;
    
    int idx = _hash(key, key_len, hash->max_slots);
    hashnode_t *node = hash->nodes[idx];

    while (node != NULL) {
        if (node->key_len == key_len && fast_cmp(get_key(node), key, key_len) == 0) {
            // [极致优化]: 如果新值长度小于等于旧值，直接原地覆写，避免触发内存分配重入
            if (val_len <= node->val_len) {
                memcpy(get_val(node), value, val_len);
                get_val(node)[val_len] = '\0';
                node->val_len = val_len;
                return 0;
            } else {
                // 如果新值太大装不下，直接走 "删除 + 重新插入" 的稳妥路线
                kvs_hash_del(hash, key, key_len);
                return kvs_hash_set(hash, key, key_len, value, val_len);
            }
        }
        node = node->next;
    }
    
    return 1; 
}

/**
 * @brief       获取当前哈希表的节点总数
 * @param[in]   hash  哈希表指针
 * @return      int   有效节点总数
 */
int kvs_hash_count(kvs_hash_t *hash) { 
    return hash ? hash->count : 0; 
}

/**
 * @brief       检查指定 Key 是否存在
 * @param[in]   hash     哈希表指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @return      int      0: 存在; 1: 不存在
 */
int kvs_hash_exist(kvs_hash_t *hash, char *key, int key_len) {
    int dummy_len = 0;
    return (kvs_hash_get(hash, key, key_len, &dummy_len) == NULL) ? 1 : 0;
}