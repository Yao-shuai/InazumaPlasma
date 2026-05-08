/**
 * @file        kvs_vector.h
 * @brief       InazumaKV 语义向量检索引擎跨语言接口层头文件
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        定义了底层 FAISS C++ 引擎暴露给纯 C 网络层的 ABI 边界。
 * 屏蔽了底层 STL 容器与面向对象机制的复杂性，通过原生 C 指针提供
 * 高维向量的初始化、写入及近似最近邻 (ANN) 检索操作。
 */

#ifndef __KVS_VECTOR_H__
#define __KVS_VECTOR_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 核心生命周期与操作接口 (Core Lifecycle & Operation APIs)
 * ========================================================================== */

/**
 * @brief       初始化向量检索引擎空间
 * @param[in]   dimension 特征向量的固定维度 (例如 OpenAI 模型的 1536 维)
 * @return      int       0: 初始化成功; 非0: 失败或已初始化
 * @note        必须在主事件循环启动前调用，用于在堆上分配 FAISS 索引实例。
 */
int kvs_vector_init(int dimension);

/**
 * @brief       向引擎中追加高维特征向量及其物理主键
 * @param[in]   key       对应业务实体的唯一键名 (需保证内存二进制安全)
 * @param[in]   key_len   键名的真实物理字节长度
 * @param[in]   vec       连续的单精度浮点数 (float) 数组指针
 * @return      int       0: 写入成功; -1: 引擎异常
 * @warning     调用侧需绝对保证传入的 vec 数组长度等于引擎初始化时声明的 dimension，
 * 否则将导致不可恢复的内存越界。
 */
int kvs_vector_add(const char* key, int key_len, const float* vec);

/**
 * @brief       执行 Top-K 近似最近邻检索，并直接格式化为 RESP 网络报文
 * @param[in]   query_vec     基准查询向量的浮点数组指针
 * @param[in]   k             期望召回的最大邻居数量 (Top-K)
 * @param[out]  response_buf  由 Reactor 层传递的 TCP 发送缓冲区指针
 * @param[in]   max_len       该缓冲区的最大可用界限
 * @return      int           实际写入 response_buf 的有效字节数
 * @note        该接口采用了零拷贝与跨层协议直写的架构设计。将计算密集型的检索过程
 * 与 I/O 密集型的报文组装在同一函数栈内闭环，避免了中间结果集对象的频繁创建与销毁。
 */
int kvs_vector_search(const float* query_vec, int k, char* response_buf, int max_len);

#ifdef __cplusplus
}
#endif

#endif // __KVS_VECTOR_H__