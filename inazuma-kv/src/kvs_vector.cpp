/**
 * @file        kvs_vector.cpp
 * @brief       InazumaKV 底层语义向量检索引擎模块
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        基于 FAISS 提供高维特征向量的内存级近似最近邻 (ANN) 检索能力。
 * 包含向量写入、欧式距离计算以及原生 RESP 协议报文的零拷贝组装。
 * 当前架构设计未引入细粒度锁，强依赖于外层的单线程 Reactor 模型。
 */

#include <faiss/IndexFlat.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include "kvs_vector.h"

/* ==========================================================================
 * 全局状态与索引实例 (Global States & Index Instances)
 * ========================================================================== */

/** @brief FAISS 全局暴力检索 L2 索引树指针 */
static faiss::IndexFlatL2* g_index = nullptr;      

/** @brief 当前运行时的向量空间维度 (例如 OpenAI text-embedding-ada-002 的 1536 维) */
static int g_dim = 0;                              

/** * @brief 向量 ID 到真实物理 Key 的反向映射表
 * @note FAISS 底层引擎仅支持 int64_t 类型的内部 ID 映射，
 * 此处通过哈希表维系内部 ID 与外部业务 Key 的关联关系。
 */
static std::unordered_map<long, std::string> g_id_to_key;

/** @brief 线性自增的全局内部标识符游标 */
static long g_current_id = 0;                      

extern "C" {

/* ==========================================================================
 * 对外暴露接口 (Public C-API Bindings)
 * ========================================================================== */

/**
 * @brief       初始化向量检索引擎的运行环境
 * @param[in]   dimension 特征向量空间的维度大小
 * @return      int       0: 初始化成功; 非0: 失败
 * @note        采用 L2 欧氏距离的暴力检索索引 (IndexFlatL2)，此模式下不执行聚类与量化，
 * 可确保检索召回率 (Recall) 达到绝对的 100%。
 */
int kvs_vector_init(int dimension) {
    /* 防御性编程：防止重复初始化导致的引擎内存泄漏 */
    if (g_index) {
        return 0;
    }
    
    g_dim = dimension;
    g_index = new faiss::IndexFlatL2(dimension);
    return 0;
}

/**
 * @brief       将文本的高维特征向量存入检索引擎
 * @param[in]   key       数据物理隔离的唯一键值 (如 idx:model:hash)
 * @param[in]   key_len   键值长度，用于绕过 C 风格字符串的 \0 截断风险
 * @param[in]   vec       连续的 float 类型向量内存数组指针
 * @return      int       0: 插入成功; -1: 引擎未初始化异常
 * @warning     传入的 vec 数组长度必须严格等于初始化时声明的 dimension，
 * 否则在底层 SIMD 指令装载时将引发严重的内存越界 (Segmentation Fault)。
 */
int kvs_vector_add(const char* key, int key_len, const float* vec) {
    if (!g_index) {
        return -1;
    }
    
    /* 直接基于指针和长度构建 std::string，实现数据边界的安全接管 */
    std::string skey(key, key_len);
    
    /* 生成并分配内部映射 ID */
    long id = g_current_id++;
    
    /* 喂入 FAISS 引擎 (单次插入 1 条，执行底层内存对齐拷贝) */
    g_index->add(1, vec); 
    
    /* 同步更新反向映射表，供后续召回查询阶段执行 ID 反查使用 */
    g_id_to_key[id] = skey; 
    
    return 0;
}

/**
 * @brief       执行 Top-K 极速向量搜索，并将结果序列化为 RESP 报文
 * @param[in]   query_vec     当前查询的基准特征向量
 * @param[in]   k             期望召回的最大结果数量 (Top-K)
 * @param[out]  response_buf  预分配的输出缓冲区指针，用于直接写入 TCP 发送列队
 * @param[in]   max_len       缓冲区的最大安全长度
 * @return      int           返回写入 response_buf 的实际有效字节数
 * @note        此函数直接跨越了传统的序列化框架，在 C++ 层面徒手拼接 RESP 协议文本。
 * 极大削减了跨语言调用及中间对象的拷贝开销，属于典型的数据面高性能实现。
 */
int kvs_vector_search(const float* query_vec, int k, char* response_buf, int max_len) {
    /* 边界条件防御：若引擎未初始化或数据为空，按 RESP 协议规范返回空数组 */
    if (!g_index || g_index->ntotal == 0) {
        const char* empty = "*0\r\n";
        memcpy(response_buf, empty, 4);
        return 4;
    }
    
    /* 动态限制最大检索范围，防止底层库因 k > ntotal 抛出异常或越界 */
    int actual_k = (g_index->ntotal < k) ? g_index->ntotal : k;
    
    /* 预分配连续内存，存放 FAISS 返回的内部 ID (I) 和对应的欧氏距离 (D) */
    std::vector<long> I(actual_k);
    std::vector<float> D(actual_k);
    
    /* 核心检索调用：触发底层 SIMD (AVX2/AVX-512) 汇编加速的向量矩阵运算 */
    g_index->search(1, query_vec, actual_k, D.data(), I.data());

    /* =========================================================================
     * RESP 报文徒手组装协议层 (Manual RESP Protocol Serialization)
     * 格式约定：*[2*K]\r\n $[Key_Len]\r\n [Key] \r\n $[Dist_Len]\r\n [Dist] \r\n
     * 语义等同于返回 [Key1, Dist1, Key2, Dist2 ...] 的交替数组。
     * ========================================================================= */
    int pos = 0;
    
    /* 写入数组头 (返回的元素数量为 Key 和 Distance 的对数，即 2 * actual_k) */
    pos += snprintf(response_buf + pos, max_len - pos, "*%d\r\n", actual_k * 2);
    
    for (int i = 0; i < actual_k; i++) {
        long id = I[i];
        
        /* 健壮性设计：映射兜底，防止内存表不一致引发 std::out_of_range 崩溃 */
        std::string res_key = g_id_to_key.count(id) ? g_id_to_key[id] : "unknown";
        
        /* 拼装 Bulk String 格式的外部物理 Key */
        pos += snprintf(response_buf + pos, max_len - pos, "$%zu\r\n%s\r\n", 
                        res_key.length(), res_key.c_str());
        
        /* 浮点数距离序列化：保留四位小数进行传输，在精度损失与网络带宽开销间取得平衡 */
        char dist_str[32];
        int d_len = snprintf(dist_str, sizeof(dist_str), "%.4f", D[i]);
        pos += snprintf(response_buf + pos, max_len - pos, "$%d\r\n%s\r\n", 
                        d_len, dist_str);
    }
    
    return pos;
}

} // extern "C"