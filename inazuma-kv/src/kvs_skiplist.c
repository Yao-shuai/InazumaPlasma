/**
 * @file        kvs_skiplist.c
 * @brief       InazumaKV 跳跃表 (Skip List) 存储引擎实现
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        采用概率型数据结构实现的有序 KV 存储基座。
 * 相比红黑树，跳表通过随机层级生成和多级链表索引，避免了复杂的树旋转开销，
 * 在高并发读写场景下具备更优的 CPU 缓存亲和性与锁粒度控制潜力。
 * 该版本已全面支持二进制安全的 Key/Value 存储。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kvstore.h"

/* ==========================================================================
 * 全局实例 (Global Instances)
 * ========================================================================== */

/** @brief 全局跳表单例实例 */
kvs_skiplist_t global_skiplist;

/* ==========================================================================
 * 内部辅助机制 (Internal Helpers)
 * ========================================================================== */

/**
 * @brief       二进制安全的比较函数
 * @param[in]   k1  指针1
 * @param[in]   l1  长度1
 * @param[in]   k2  指针2
 * @param[in]   l2  长度2
 * @return      int <0: k1小; >0: k1大; ==0: 完全相等
 * @note        采用按最短长度前缀匹配，若前缀相同则较短的字节序列被认定为更小。
 * 保障任意二进制流 (如 Protobuf/序列化对象) 的严格全序排序。
 */
static inline int kvs_bincmp(const char *k1, int l1, const char *k2, int l2) {
    int min_len = (l1 < l2) ? l1 : l2;
    int cmp = memcmp(k1, k2, min_len);
    if (cmp == 0) {
        return l1 - l2; 
    }
    return cmp;
}

/**
 * @brief       创建跳表节点并执行二进制安全深拷贝
 * @param[in]   level    当前节点分配的随机索引层数
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @param[in]   value    值数据
 * @param[in]   val_len  值长度
 * @return      kvs_skiplist_node_t* 初始化完成的节点指针；OOM 返回 NULL
 * @note        包含防御性内存分配：为保证 C 风格字符串兼容性，底层实际分配长度为 len + 1。
 * 前向指针数组 (forward) 的大小由 level 动态决定。
 */
static kvs_skiplist_node_t* _create_node(int level, const char* key, int key_len, const char* value, int val_len) {
    kvs_skiplist_node_t* node = (kvs_skiplist_node_t*)kvs_malloc(sizeof(kvs_skiplist_node_t));
    if (!node) return NULL;

    node->key = kvs_malloc(key_len + 1);
    node->value = kvs_malloc(val_len + 1);
    
    if (!node->key || !node->value) {
        if (node->key) kvs_free(node->key);
        if (node->value) kvs_free(node->value);
        kvs_free(node);
        return NULL;
    }

    /* 内存安全拷贝与封口 */
    memcpy(node->key, key, key_len);
    node->key[key_len] = '\0';
    node->key_len = key_len;

    memcpy(node->value, value, val_len);
    node->value[val_len] = '\0';
    node->val_len = val_len;
    
    /* 动态分配跨层前向指针数组 */
    node->forward = (kvs_skiplist_node_t**)kvs_malloc((level + 1) * sizeof(kvs_skiplist_node_t*));
    if (!node->forward) {
        kvs_free(node->key);
        kvs_free(node->value);
        kvs_free(node);
        return NULL;
    }

    for (int i = 0; i <= level; i++) {
        node->forward[i] = NULL;
    }
    
    return node;
}

/**
 * @brief       安全释放跳表节点持有的所有动态内存
 * @param[in]   node  待释放的节点指针
 */
static void _free_node(kvs_skiplist_node_t* node) {
    if (node) {
        if (node->key) kvs_free(node->key);
        if (node->value) kvs_free(node->value);
        if (node->forward) kvs_free(node->forward);
        kvs_free(node);
    }
}

/**
 * @brief       基于几何分布 (Geometric Distribution) 生成随机层级
 * @return      int 生成的层级 (范围: 0 ~ KVS_SKIPLIST_MAXLEVEL - 1)
 * @note        使用 p=0.5 的概率因子。每升高一层，概率减半。
 * 决定了跳表在时间和空间复杂度上的宏观表现。
 */
static int _random_level() {
    int level = 0;
    while (rand() < RAND_MAX / 2 && level < KVS_SKIPLIST_MAXLEVEL) {
        level++;
    }
    return level;
}

/* ==========================================================================
 * 对外暴露接口 (Public KV APIs)
 * ========================================================================== */

/**
 * @brief       初始化跳表实例
 * @param[in]   inst  待初始化的跳表指针
 * @return      int   0: 成功; -1: 参数错误或内存分配失败
 */
int kvs_skiplist_create(kvs_skiplist_t *inst) {
    if (inst == NULL) return -1;
    
    srand((unsigned int)time(NULL));
    inst->level = 0;
    
    /* 创建哨兵头节点 (Key/Value 长度为 0，视为最小边界) */
    inst->header = _create_node(KVS_SKIPLIST_MAXLEVEL, "", 0, "", 0);
    if (!inst->header) return -1;

    inst->count = 0;
    return 0;
}

/**
 * @brief       销毁跳表实例并释放所有节点内存
 * @param[in]   inst  待销毁的跳表指针
 */
void kvs_skiplist_destory(kvs_skiplist_t *inst) {
    if (inst == NULL || inst->header == NULL) return;

    kvs_skiplist_node_t* node = inst->header->forward[0];
    while (node != NULL) {
        kvs_skiplist_node_t* next = node->forward[0];
        _free_node(node);
        node = next;
    }
    
    _free_node(inst->header);
    inst->header = NULL;
}

/**
 * @brief       向跳表中插入或覆盖 Key-Value 对
 * @param[in]   inst     跳表实例指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @param[in]   value    值数据
 * @param[in]   val_len  值长度
 * @return      int      0: 成功; 1: 键已存在; -1: 参数错误; -2: OOM
 */
int kvs_skiplist_set(kvs_skiplist_t *inst, char *key, int key_len, char *value, int val_len) {
    if (!inst || !key || !value) return -1;

    kvs_skiplist_node_t* update[KVS_SKIPLIST_MAXLEVEL + 1];
    kvs_skiplist_node_t* curr = inst->header;

    /* 从最高层向下寻找插入位置，并记录每层的更新点 */
    for (int i = inst->level; i >= 0; i--) {
        while (curr->forward[i] != NULL && 
               kvs_bincmp(curr->forward[i]->key, curr->forward[i]->key_len, key, key_len) < 0) {
            curr = curr->forward[i];
        }
        update[i] = curr;
    }

    curr = curr->forward[0];

    /* 若节点已存在，依据当前语义返回状态码，交由上层逻辑判断是否走 Mod */
    if (curr != NULL && curr->key_len == key_len && memcmp(curr->key, key, key_len) == 0) {
        return 1; 
    }

    /* 生成随机层级并可能提升整个跳表的最高层级限制 */
    int lvl = _random_level();
    if (lvl > inst->level) {
        for (int i = inst->level + 1; i <= lvl; i++) {
            update[i] = inst->header;
        }
        inst->level = lvl;
    }

    kvs_skiplist_node_t* newNode = _create_node(lvl, key, key_len, value, val_len);
    if (!newNode) return -2;

    /* 执行链表指针接驳操作 */
    for (int i = 0; i <= lvl; i++) {
        newNode->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = newNode;
    }
    
    inst->count++; 
    return 0; 
}

/**
 * @brief       获取跳表中指定 Key 的 Value
 * @param[in]   inst     跳表实例指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @param[out]  out_vlen 用于传出真实值长度的指针
 * @return      char* 查找到的 Value 首地址；未找到返回 NULL
 */
char* kvs_skiplist_get(kvs_skiplist_t *inst, char *key, int key_len, int *out_vlen) {
    if (!inst || !key) return NULL;

    kvs_skiplist_node_t* curr = inst->header;
    for (int i = inst->level; i >= 0; i--) {
        while (curr->forward[i] != NULL && 
               kvs_bincmp(curr->forward[i]->key, curr->forward[i]->key_len, key, key_len) < 0) {
            curr = curr->forward[i];
        }
    }

    curr = curr->forward[0];
    
    /* 采用 memcmp 实施最终的精确校验 */
    if (curr != NULL && curr->key_len == key_len && memcmp(curr->key, key, key_len) == 0) {
        if (out_vlen) *out_vlen = curr->val_len;
        return curr->value;
    }
    
    return NULL;
}

/**
 * @brief       删除跳表中指定的 Key-Value 对
 * @param[in]   inst     跳表实例指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @return      int      0: 删除成功; 1: 键不存在; -1: 参数错误
 */
int kvs_skiplist_del(kvs_skiplist_t *inst, char *key, int key_len) {
    if (!inst || !key) return -1;

    kvs_skiplist_node_t* update[KVS_SKIPLIST_MAXLEVEL + 1];
    kvs_skiplist_node_t* curr = inst->header;

    for (int i = inst->level; i >= 0; i--) {
        while (curr->forward[i] != NULL && 
               kvs_bincmp(curr->forward[i]->key, curr->forward[i]->key_len, key, key_len) < 0) {
            curr = curr->forward[i];
        }
        update[i] = curr;
    }

    curr = curr->forward[0];

    if (curr != NULL && curr->key_len == key_len && memcmp(curr->key, key, key_len) == 0) {
        for (int i = 0; i <= inst->level; i++) {
            if (update[i]->forward[i] != curr) break;
            update[i]->forward[i] = curr->forward[i];
        }

        _free_node(curr);

        /* 若删除的节点含有最高层索引，则对应收缩跳表的最高层级限制 */
        while (inst->level > 0 && inst->header->forward[inst->level] == NULL) {
            inst->level--;
        }
        
        inst->count--;
        return 0; 
    }

    return 1; 
}

/**
 * @brief       修改跳表中指定 Key 的 Value
 * @param[in]   inst     跳表实例指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @param[in]   value    新的值数据
 * @param[in]   val_len  新的值长度
 * @return      int      0: 修改成功; 1: 键不存在; -1: 参数错误
 */
int kvs_skiplist_mod(kvs_skiplist_t *inst, char *key, int key_len, char *value, int val_len) {
    if (!inst || !key || !value) return -1;

    kvs_skiplist_node_t* curr = inst->header;
    for (int i = inst->level; i >= 0; i--) {
        while (curr->forward[i] != NULL && 
               kvs_bincmp(curr->forward[i]->key, curr->forward[i]->key_len, key, key_len) < 0) {
            curr = curr->forward[i];
        }
    }

    curr = curr->forward[0];
    
    if (curr != NULL && curr->key_len == key_len && memcmp(curr->key, key, key_len) == 0) {
        if (curr->value) kvs_free(curr->value);
        
        curr->value = kvs_malloc(val_len + 1);
        memcpy(curr->value, value, val_len);
        curr->value[val_len] = '\0';
        curr->val_len = val_len;
        
        return 0; 
    }

    return 1; 
}

/**
 * @brief       检查指定 Key 在跳表中是否存在
 * @param[in]   inst     跳表实例指针
 * @param[in]   key      键数据
 * @param[in]   key_len  键长度
 * @return      int      0: 存在; 1: 不存在; -1: 参数错误
 */
int kvs_skiplist_exist(kvs_skiplist_t *inst, char *key, int key_len) {
    if (!inst || !key) return -1;
    
    int dummy_len = 0;
    if (kvs_skiplist_get(inst, key, key_len, &dummy_len) != NULL) {
        return 0; 
    }
    return 1; 
}

/**
 * @brief       获取跳表中的有效节点总数
 * @param[in]   inst  跳表实例指针
 * @return      int   节点总数
 */
int kvs_skiplist_count(kvs_skiplist_t *inst) {
    return inst ? inst->count : 0;
}