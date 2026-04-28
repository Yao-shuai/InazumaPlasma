/**
 * @file        kvs_rbtree.c
 * @brief       InazumaKV 红黑树存储引擎实现 (二进制安全版)
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        采用红黑树实现的高性能有序 KV 存储基座。
 * 核心改造点：通过 kvs_bincmp 和动态内存分配，完美支持二进制安全的 Key/Value 存储。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kvstore.h"

/* ==========================================================================
 * 底层比较器 (Internal Comparators)
 * ========================================================================== */

/**
 * @brief       [核心改造] 二进制安全的比较函数 (替代传统的 strcmp)
 * @param[in]   k1  指针1
 * @param[in]   l1  长度1
 * @param[in]   k2  指针2
 * @param[in]   l2  长度2
 * @return      int <0: k1小; >0: k1大; ==0: 完全相等
 * @note        先按最短长度进行 memcmp 比较，如果前缀完全相同，较短的字符串算“小”。
 * 这保证了任意二进制流都可以作为红黑树的 Key 进行严格全序排序。
 */
static inline int kvs_bincmp(const char *k1, int l1, const char *k2, int l2) {
    int min_len = (l1 < l2) ? l1 : l2;
    int cmp = memcmp(k1, k2, min_len);
    if (cmp == 0) {
        return l1 - l2; 
    }
    return cmp;
}


/* ==========================================================================
 * 红黑树基础操作与旋转逻辑 (Rbtree Primitives & Rotations)
 * ========================================================================== */

/** @brief 查找子树的最小节点 */
rbtree_node *rbtree_mini(rbtree *T, rbtree_node *x) {
    while (x->left != T->nil) x = x->left;
    return x;
}

/** @brief 查找子树的最大节点 */
rbtree_node *rbtree_maxi(rbtree *T, rbtree_node *x) {
    while (x->right != T->nil) x = x->right;
    return x;
}

/** @brief 查找节点的中序遍历后继节点 */
rbtree_node *rbtree_successor(rbtree *T, rbtree_node *x) {
    rbtree_node *y = x->parent;
    if (x->right != T->nil) return rbtree_mini(T, x->right);
    while ((y != T->nil) && (x == y->right)) { 
        x = y; 
        y = y->parent; 
    }
    return y;
}

/** @brief 对节点进行左旋操作，维护二叉查找树性质 */
void rbtree_left_rotate(rbtree *T, rbtree_node *x) {
    rbtree_node *y = x->right;  
    x->right = y->left; 
    
    if (y->left != T->nil) y->left->parent = x;
    y->parent = x->parent; 
    
    if (x->parent == T->nil) T->root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    
    y->left = x; 
    x->parent = y; 
}

/** @brief 对节点进行右旋操作，维护二叉查找树性质 */
void rbtree_right_rotate(rbtree *T, rbtree_node *y) {
    rbtree_node *x = y->left;
    y->left = x->right;
    
    if (x->right != T->nil) x->right->parent = y;
    x->parent = y->parent;
    
    if (y->parent == T->nil) T->root = x;
    else if (y == y->parent->right) y->parent->right = x;
    else y->parent->left = x;
    
    x->right = y;
    y->parent = x;
}


/* ==========================================================================
 * 红黑树核心维护逻辑 (Insertion & Deletion Fixups)
 * ========================================================================== */

/**
 * @brief 插入节点后的颜色修正，恢复红黑树的五大性质
 */
void rbtree_insert_fixup(rbtree *T, rbtree_node *z) {
    while (z->parent->color == RED) { 
        if (z->parent == z->parent->parent->left) {
            rbtree_node *y = z->parent->parent->right;
            if (y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent; 
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rbtree_left_rotate(T, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rbtree_right_rotate(T, z->parent->parent);
            }
        } else {
            rbtree_node *y = z->parent->parent->left;
            if (y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent; 
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rbtree_right_rotate(T, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rbtree_left_rotate(T, z->parent->parent);
            }
        }
    }
    T->root->color = BLACK;
}

/**
 * @brief 将新节点插入红黑树
 */
void rbtree_insert(rbtree *T, rbtree_node *z) {
    rbtree_node *y = T->nil;
    rbtree_node *x = T->root;

    while (x != T->nil) {
        y = x;
#if ENABLE_KEY_CHAR
        // [核心改造 1] 使用 kvs_bincmp 替代 strcmp
        int cmp = kvs_bincmp((char*)z->key, z->key_len, (char*)x->key, x->key_len);
        if (cmp < 0) x = x->left;
        else if (cmp > 0) x = x->right;
        else return; // 拦截重复插入
#else
        if (z->key < x->key) x = x->left;
        else if (z->key > x->key) x = x->right;
        else return;
#endif
    }

    z->parent = y;
    if (y == T->nil) {
        T->root = z;
#if ENABLE_KEY_CHAR
    // [核心改造 2] 使用 kvs_bincmp 替代 strcmp
    } else if (kvs_bincmp((char*)z->key, z->key_len, (char*)y->key, y->key_len) < 0) {
#else
    } else if (z->key < y->key) {
#endif
        y->left = z;
    } else {
        y->right = z;
    }

    z->left = T->nil;
    z->right = T->nil;
    z->color = RED;

    rbtree_insert_fixup(T, z);
}

/**
 * @brief 删除节点后的颜色修正，恢复红黑树性质
 * @note  已将原本高度压缩的单行代码进行大厂规范化展平，便于运维调试与接手。
 */
void rbtree_delete_fixup(rbtree *T, rbtree_node *x) {
    while ((x != T->root) && (x->color == BLACK)) {
        if (x == x->parent->left) {
            rbtree_node *w = x->parent->right;
            if (w->color == RED) { 
                w->color = BLACK; 
                x->parent->color = RED; 
                rbtree_left_rotate(T, x->parent); 
                w = x->parent->right; 
            }
            if ((w->left->color == BLACK) && (w->right->color == BLACK)) { 
                w->color = RED; 
                x = x->parent; 
            } else {
                if (w->right->color == BLACK) { 
                    w->left->color = BLACK; 
                    w->color = RED; 
                    rbtree_right_rotate(T, w); 
                    w = x->parent->right; 
                }
                w->color = x->parent->color; 
                x->parent->color = BLACK; 
                w->right->color = BLACK; 
                rbtree_left_rotate(T, x->parent); 
                x = T->root;
            }
        } else {
            rbtree_node *w = x->parent->left;
            if (w->color == RED) { 
                w->color = BLACK; 
                x->parent->color = RED; 
                rbtree_right_rotate(T, x->parent); 
                w = x->parent->left; 
            }
            if ((w->left->color == BLACK) && (w->right->color == BLACK)) { 
                w->color = RED; 
                x = x->parent; 
            } else {
                if (w->left->color == BLACK) { 
                    w->right->color = BLACK; 
                    w->color = RED; 
                    rbtree_left_rotate(T, w); 
                    w = x->parent->left; 
                }
                w->color = x->parent->color; 
                x->parent->color = BLACK; 
                w->left->color = BLACK; 
                rbtree_right_rotate(T, x->parent); 
                x = T->root;
            }
        }
    }
    x->color = BLACK;
}

/**
 * @brief 从红黑树中删除节点
 */
rbtree_node *rbtree_delete(rbtree *T, rbtree_node *z) {
    rbtree_node *y = T->nil;
    rbtree_node *x = T->nil;

    if ((z->left == T->nil) || (z->right == T->nil)) y = z;
    else y = rbtree_successor(T, z);

    if (y->left != T->nil) x = y->left;
    else if (y->right != T->nil) x = y->right;

    x->parent = y->parent;
    if (y->parent == T->nil) T->root = x;
    else if (y == y->parent->left) y->parent->left = x;
    else y->parent->right = x;

    if (y != z) {
#if ENABLE_KEY_CHAR
        // [核心改造 3] 交换节点内容时，一并交换真实数据与长度 (浅拷贝指针)
        void *tmp = z->key;
        int tmp_len = z->key_len;
        z->key = y->key;
        z->key_len = y->key_len;
        y->key = tmp;
        y->key_len = tmp_len;

        tmp = z->value;
        tmp_len = z->val_len;
        z->value= y->value;
        z->val_len = y->val_len;
        y->value = tmp;
        y->val_len = tmp_len;
#else
        z->key = y->key;
        z->value = y->value;
#endif
    }
    if (y->color == BLACK) rbtree_delete_fixup(T, x);
    
    return y;
}

/**
 * @brief       [核心改造 4] 二进制安全搜索
 * @param[in]   key      搜索的目标键
 * @param[in]   key_len  目标键的长度
 * @return      rbtree_node* 命中节点，未命中返回 T->nil
 */
rbtree_node *rbtree_search(rbtree *T, char* key, int key_len) {
    rbtree_node *node = T->root;
    while (node != T->nil) {
#if ENABLE_KEY_CHAR
        int cmp = kvs_bincmp(key, key_len, (char*)node->key, node->key_len);
        if (cmp < 0) node = node->left;
        else if (cmp > 0) node = node->right;
        else return node;
#else
        // ... (数字类型逻辑略)
#endif
    }
    return T->nil;
}


/* ==========================================================================
 * 对外暴露接口 (Public KV APIs)
 * ========================================================================== */

typedef struct _rbtree kvs_rbtree_t; 

/** @brief 全局红黑树单例 */
kvs_rbtree_t global_rbtree;

/**
 * @brief       初始化红黑树实例 (建立哨兵节点)
 */
int kvs_rbtree_create(kvs_rbtree_t *inst) {
    if (inst == NULL) return -1;
    
    inst->nil = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
    if (!inst->nil) return -1;
    
    // 初始化哨兵 (Sentinel)
    inst->nil->color = BLACK;
    inst->nil->left = NULL; 
    inst->nil->right = NULL;
    inst->nil->parent = NULL;
    inst->nil->key = NULL;  
    inst->nil->value = NULL;
    inst->nil->key_len = 0; // [新增]
    inst->nil->val_len = 0; // [新增]
    
    inst->root = inst->nil;
    inst->count = 0;        // [新增]
    
    return 0;
}

/**
 * @brief       销毁红黑树实例并安全释放所有堆内存
 * @note        采用后序遍历的思想不断删除最小值，直到清空。
 */
void kvs_rbtree_destory(kvs_rbtree_t *inst) {
    if (inst == NULL) return;
    
    while (inst->root != inst->nil) {
        rbtree_node *mini = rbtree_mini(inst, inst->root);
        rbtree_node *cur = rbtree_delete(inst, mini);
        
        if (cur->key) kvs_free(cur->key);
        if (cur->value) kvs_free(cur->value);
        kvs_free(cur);
    }
    kvs_free(inst->nil);
}


/* ==========================================================================
 * [核心改造 5] 业务 API 适配二进制安全
 * ========================================================================== */

/**
 * @brief       向树中插入新的 Key-Value 对
 */
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, int key_len, char *value, int val_len) {
    if (!inst || !key || !value) return -1;

    // 先查重
    rbtree_node *node = rbtree_search(inst, key, key_len);
    if (node != inst->nil) return 1;

    rbtree_node *new_node = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
    if (!new_node) return -2;

    // 分配内存时 +1 留作 '\0'，使用 memcpy 进行安全的二进制深拷贝
    new_node->key = kvs_malloc(key_len + 1);
    memcpy(new_node->key, key, key_len);
    ((char*)new_node->key)[key_len] = '\0';
    new_node->key_len = key_len;

    new_node->value = kvs_malloc(val_len + 1);
    memcpy(new_node->value, value, val_len);
    ((char*)new_node->value)[val_len] = '\0';
    new_node->val_len = val_len;

    rbtree_insert(inst, new_node);
    inst->count++; // [新增] 插入成功后计数器累加
    
    return 0;
}

/**
 * @brief       获取指定的 Value
 */
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key, int key_len, int *out_vlen) {
    if (!inst || !key) return NULL;
    
    rbtree_node *node = rbtree_search(inst, key, key_len);
    if (node == inst->nil) return NULL; 

    // 安全传出真实的 Value 长度
    if (out_vlen) *out_vlen = node->val_len;
    return (char*)node->value;
}

/**
 * @brief       删除指定的 Key-Value 对
 */
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key, int key_len) {
    if (!inst || !key) return -1;

    rbtree_node *node = rbtree_search(inst, key, key_len);
    if (node == inst->nil) return 1; 
    
    // 执行红黑树删除逻辑
    rbtree_node *cur = rbtree_delete(inst, node);

    // 释放深拷贝的堆内存
    if (cur->key) kvs_free(cur->key);
    if (cur->value) kvs_free(cur->value);
    kvs_free(cur);
    
    inst->count--; // [新增] 递减计数器
    return 0;
}

/**
 * @brief       修改指定 Key 的 Value 数据
 */
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, int key_len, char *value, int val_len) {
    if (!inst || !key || !value) return -1;

    rbtree_node *node = rbtree_search(inst, key, key_len);
    if (node == inst->nil) return 1; 

    // 释放旧值，开辟新空间并深拷贝
    if (node->value) kvs_free(node->value);
    
    node->value = kvs_malloc(val_len + 1);
    memcpy(node->value, value, val_len);
    ((char*)node->value)[val_len] = '\0';
    node->val_len = val_len;

    return 0;
}

/**
 * @brief       判断指定的 Key 是否存在
 */
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key, int key_len) {
    if (!inst || !key) return -1;
    
    rbtree_node *node = rbtree_search(inst, key, key_len);
    return (node != inst->nil) ? 0 : 1;
}

/**
 * @brief       获取红黑树中的元素总数
 */
int kvs_rbtree_count(kvs_rbtree_t *inst) {
    return inst ? inst->count : 0;
}