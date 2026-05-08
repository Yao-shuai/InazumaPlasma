/**
 * @file        kvs_array.c
 * @brief       InazumaKV 数组存储引擎实现
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        采用固定容量的数组实现的键值存储结构，支持二进制安全（Binary Safe）。
 * 通过预留空洞机制（first_free_idx）实现空间复用。
 */

#include "kvstore.h"
#include <string.h> 
#include <stdlib.h>

/**
 * @brief       全局数组存储单例实例
 */
kvs_array_t global_array = {0};

/**
 * @brief       初始化 KV 数组实例
 * @param[in]   inst  指向待初始化的 kvs_array_t 实例的指针
 * @return      int   0: 成功; -1: 失败 (指针为空或已初始化)
 */
int kvs_array_create(kvs_array_t *inst) {
    if (!inst) return -1;
    if (inst->table) {
        return -1;
    }   
    
    inst->table = kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    if (!inst->table) {
        return -1;
    }
    
    // 初始化清零，防止脏数据导致野指针解引用
    memset(inst->table, 0, KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    inst->total = 0;
    inst->count = 0;
    
    return 0;
}

/**
 * @brief       销毁 KV 数组实例，释放所有堆内存
 * @param[in]   inst  指向待销毁的 kvs_array_t 实例的指针
 * @note        函数名拼写存在历史遗留 (destory -> destroy)，为兼容旧接口暂不修改。
 */
void kvs_array_destory(kvs_array_t *inst) {
    if (!inst) return;
    
    if (inst->table) {
        // 遍历释放所有 item 内部持有的 Key/Value 内存
        for (int i = 0; i < inst->total; i++) {
            if (inst->table[i].key) kvs_free(inst->table[i].key);
            if (inst->table[i].value) kvs_free(inst->table[i].value);
        }
        
        // 释放主表内存并防范悬空指针
        kvs_free(inst->table);
        inst->table = NULL;
    }
}

/**
 * @brief       向数组中插入或覆盖一个 Key-Value 对 (二进制安全支持)
 * @param[in]   inst     数组实例指针
 * @param[in]   key      传入的键 (二进制数据)
 * @param[in]   key_len  键的真实字节长度
 * @param[in]   value    传入的值 (二进制数据)
 * @param[in]   val_len  值的真实字节长度
 * @return      int      0: 插入成功; 1: 键已存在; -1: 参数错误/表满; -2: OOM 内存不足
 */
int kvs_array_set(kvs_array_t *inst, char *key, int key_len, char *value, int val_len) {
    if (inst == NULL || key == NULL || value == NULL) return -1;

    int first_free_idx = -1;
    
    // 1. 遍历比对，同时记录第一个可用空洞
    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key == NULL) {
            if (first_free_idx == -1) first_free_idx = i; // 记录第一个空洞位置，优化后续插入
            continue;
        }

        // [二进制安全核心] 先比对长度，提升不命中时的判断效率；再用 memcmp 比较内存块内容
        if (inst->table[i].key_len == key_len && 
            memcmp(inst->table[i].key, key, key_len) == 0) {
            return 1; // 已存在，按当前逻辑不执行覆写，直接返回
        }
    }

    // 2. 寻找插入点
    int idx = -1;
    if (first_free_idx != -1) {
        idx = first_free_idx; // 优先复用删除留下的空洞
    } else {
        if (inst->total >= KVS_ARRAY_SIZE) return -1; // 数组容量已满
        idx = inst->total;
        inst->total++; // 只有在尾部追加时才增加 total 高水位线
    }

    // 3. 内存分配与拷贝
    // 额外分配 1 字节用于存放 '\0'，即使是二进制数据，也能兼容传统的 C 字符串打印函数
    inst->table[idx].key = kvs_malloc(key_len + 1);
    inst->table[idx].value = kvs_malloc(val_len + 1);

    if (!inst->table[idx].key || !inst->table[idx].value) {
        // [防御性编程] 发生 OOM 时回滚已分配的内存，防止内存泄漏
        if (inst->table[idx].key) kvs_free(inst->table[idx].key);
        if (inst->table[idx].value) kvs_free(inst->table[idx].value);
        return -2; 
    }

    // 拷贝二进制数据，并严格记录真实长度
    memcpy(inst->table[idx].key, key, key_len);
    inst->table[idx].key[key_len] = '\0';
    inst->table[idx].key_len = key_len;

    memcpy(inst->table[idx].value, value, val_len);
    inst->table[idx].value[val_len] = '\0';
    inst->table[idx].val_len = val_len;

    inst->count++;
    return 0;
}

/**
 * @brief       从数组中获取指定的 Value (二进制安全支持)
 * @param[in]   inst     数组实例指针
 * @param[in]   key      要查找的键
 * @param[in]   key_len  键的真实字节长度
 * @param[out]  out_vlen 用于传出底层真实 Value 长度的指针
 * @return      char* 查找到的 Value 首地址；未找到返回 NULL
 * @warning     返回的指针直接指向底层内存，调用方不应执行 free 或修改操作。
 */
char* kvs_array_get(kvs_array_t *inst, char *key, int key_len, int *out_vlen) {
    if (inst == NULL || key == NULL) return NULL;

    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key == NULL) {
            continue;
        }
        
        // memcmp 精确匹配二进制内存
        if (inst->table[i].key_len == key_len && 
            memcmp(inst->table[i].key, key, key_len) == 0) {
            
            // 安全传出真实的 Value 长度
            if (out_vlen) *out_vlen = inst->table[i].val_len; 
            return inst->table[i].value;
        }
    }
    return NULL;
}

/**
 * @brief       从数组中删除指定的 Key-Value 对
 * @param[in]   inst     数组实例指针
 * @param[in]   key      要删除的键
 * @param[in]   key_len  键的真实字节长度
 * @return      int      0: 删除成功; 1: 键不存在; -1: 参数错误
 */
int kvs_array_del(kvs_array_t *inst, char *key, int key_len) {
    if (inst == NULL || key == NULL) return -1;

    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key == NULL) continue;

        if (inst->table[i].key_len == key_len && 
            memcmp(inst->table[i].key, key, key_len) == 0) {
            
            // 释放内存
            kvs_free(inst->table[i].key);
            kvs_free(inst->table[i].value);
            
            // 置空指针和长度，形成 "空洞"，供下次 set 复用
            inst->table[i].key = NULL; 
            inst->table[i].value = NULL;
            inst->table[i].key_len = 0;
            inst->table[i].val_len = 0;

            // 仅当删除的是数组最尾部的元素时，才收缩高水位线 total
            if (i == inst->total - 1) {
                inst->total--;
            }
            inst->count--;
            return 0;
        }
    }
    return 1; // Not exist
}

/**
 * @brief       修改指定 Key 的 Value 数据
 * @param[in]   inst     数组实例指针
 * @param[in]   key      目标键
 * @param[in]   key_len  键长度
 * @param[in]   value    新的值数据
 * @param[in]   val_len  新值长度
 * @return      int      0: 修改成功; 1: 键未找到; -1: 参数错误
 */
int kvs_array_mod(kvs_array_t *inst, char *key, int key_len, char *value, int val_len) {
    if (inst == NULL || key == NULL || value == NULL) return -1;

    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key == NULL) continue;

        if (inst->table[i].key_len == key_len && 
            memcmp(inst->table[i].key, key, key_len) == 0) {
            
            // 释放旧值内存
            kvs_free(inst->table[i].value); 
            
            // 重新分配并拷贝新值
            // [@note] 工业级严谨性提醒: 实际生产中此处若 kvs_malloc 失败 (OOM)，
            // 会导致当前 Key 的 value 变成 NULL 野指针。此处暂按原逻辑保留。
            inst->table[i].value = kvs_malloc(val_len + 1);
            memcpy(inst->table[i].value, value, val_len);
            inst->table[i].value[val_len] = '\0';
            inst->table[i].val_len = val_len;
            
            return 0;
        }
    }
    return 1; // Not found
}

/**
 * @brief       检查指定 Key 是否存在
 * @param[in]   inst     数组实例指针
 * @param[in]   key      要检查的键
 * @param[in]   key_len  键的字节长度
 * @return      int      0: 存在; 1: 不存在; -1: 参数错误
 */
int kvs_array_exist(kvs_array_t *inst, char *key, int key_len) {
    if (!inst || !key) return -1;
    
    int dummy_len = 0;
    // 复用 get 函数进行检索
    char *str = kvs_array_get(inst, key, key_len, &dummy_len);
    return (str == NULL) ? 1 : 0;
}

/**
 * @brief       获取当前存储的有效键值对总数
 * @param[in]   inst     数组实例指针
 * @return      int      有效元素数量 (count)
 */
int kvs_array_count(kvs_array_t *inst) {
    return inst ? inst->count : 0;
}