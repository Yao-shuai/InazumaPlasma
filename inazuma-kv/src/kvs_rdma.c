/**
 * @file        kvs_rdma.c
 * @brief       基于 RoCEv2 (RDMA) 的多并发流水线直写层
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        该模块利用 RDMA 技术实现跨节点的内存零拷贝传输 (Kernel Bypass)。
 * 核心架构采用 Pipelining (流水线) 结合 IBV_WR_RDMA_WRITE_WITH_IMM 机制，
 * 将大块内存切割为指定大小的 Chunk 并发推送，以填满网卡带宽并降低单次 DMA 延迟。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

/* ==========================================================================
 * 宏定义与数据结构 (Macros & Data Structures)
 * ========================================================================== */

#define RDMA_PORT "20886"

/* 切割成 512KB 的数据块进行并发传输，规避底层网卡单次 DMA 的尺寸限制与拥塞 */
#define CHUNK_SIZE (512 * 1024) 

/* 发送队列 (SQ) 的滑动窗口流水线深度，控制 In-flight Work Requests 数量 */
#define QP_DEPTH 16             

/**
 * @brief RDMA 内存区域描述符，用于在通信对端间交换远程访问凭证
 */
struct rdma_mem_info {
    uint64_t addr;      /* 内存区域起始物理/虚拟地址 */
    uint32_t rkey;      /* 远程访问密钥 (Remote Key) */
};

/* ==========================================================================
 * 内部辅助机制 (Internal Helpers)
 * ========================================================================== */

/**
 * @brief       阻塞轮询完成队列 (Completion Queue)
 * @param[in]   cq  需要轮询的 ibv_cq 实例
 * @return      int 0: 成功获取成功的 Work Completion; -1: 轮询失败或状态异常
 * @note        由于采用纯轮询模型 (Polling)，此函数会持续占用 CPU 资源直至事件到达，
 * 适用于对延迟要求极高的微秒级网络栈。
 */
static int poll_completion(struct ibv_cq *cq) {
    struct ibv_wc wc;
    int n;
    do { 
        n = ibv_poll_cq(cq, 1, &wc); 
    } while (n == 0); 
    
    if (n < 0 || wc.status != IBV_WC_SUCCESS) {
        return -1;
    }
    return 0;
}

/* ==========================================================================
 * 核心对外接口：RDMA 数据面传输 (Core RDMA Data Plane APIs)
 * ========================================================================== */

/**
 * @brief       Master 发送端：基于流水线的内存块并发直写 (Write-Through)
 * @param[in]   target_ip 目标 Slave 节点的 IPv4 地址
 * @param[in]   buffer    待发送数据的本地内存基址
 * @param[in]   size      待发送数据的总字节数
 * @param[out]  cost      传出参数，记录纯网络传输阶段的耗时 (ms)
 * @return      int       0: 传输成功; -1: 链路建立或传输失败
 * @note        采用滑动窗口机制控制并发度。前 N-1 个包使用 IBV_WR_RDMA_WRITE 
 * 进行静默单向写入，最后一个包挂载 IB_WR_RDMA_WRITE_WITH_IMM 触发对端中断响应。
 */
int kvs_rdma_send_memory(const char *target_ip, void *buffer, size_t size, double *cost) {
    struct rdma_addrinfo hints, *res;
    struct rdma_cm_id *id;
    struct ibv_mr *mr, *info_mr;
    struct rdma_mem_info remote_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    if (rdma_getaddrinfo(target_ip, RDMA_PORT, &hints, &res)) return -1;

    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    /* 配置 Queue Pair 容量，发送队列深度需足以支撑滑动窗口大小 */
    attr.cap.max_send_wr = QP_DEPTH * 2; 
    attr.cap.max_recv_wr = 16;
    attr.cap.max_send_sge = 1; 
    attr.cap.max_recv_sge = 1;
    attr.sq_sig_all = 1;

    if (rdma_create_ep(&id, res, NULL, &attr)) return -1;
    rdma_freeaddrinfo(res);

    /* 独立计时：评估内存注册 (Memory Registration, MR) 造成的内核态上下文开销 */
    struct timeval mr_t1, mr_t2;
    gettimeofday(&mr_t1, NULL);
    mr = rdma_reg_msgs(id, buffer, size);
    info_mr = rdma_reg_msgs(id, &remote_info, sizeof(remote_info));
    gettimeofday(&mr_t2, NULL);
    double mr_cost = (mr_t2.tv_sec - mr_t1.tv_sec) * 1000.0 + (mr_t2.tv_usec - mr_t1.tv_usec) / 1000.0;
    printf("[RDMA] Memory Registration Overhead: %.2f ms (To be eliminated via Global MR Pool)\n", mr_cost);

    /* 预投递 Receive Request 以接收对端返回的 R_Key */
    rdma_post_recv(id, NULL, &remote_info, sizeof(remote_info), info_mr);

    if (rdma_connect(id, NULL)) return -1;
    if (poll_completion(id->recv_cq) < 0) return -1;

    /* 精确计时：纯网络层硬件传输阶段 */
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);
    
    /* 核心机制：基于 Pipelining 的分块并发传输 */
    int num_chunks = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int unpolled = 0;

    for (int i = 0; i < num_chunks; i++) {
        size_t cur_len = (i == num_chunks - 1) ? (size - i * CHUNK_SIZE) : CHUNK_SIZE;
        
        struct ibv_sge sge = { 
            .addr = (uintptr_t)buffer + i * CHUNK_SIZE, 
            .length = cur_len, 
            .lkey = mr->lkey 
        };
        struct ibv_send_wr wr = {0}, *bad_wr;
        
        wr.wr_id = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        
        if (i == num_chunks - 1) {
            /* 最后一个分块：挂载 Immediate Data 以触发接收端 CQE */
            wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM; 
            wr.imm_data = htonl(0x8888); 
        } else {
            /* 常规分块：纯单向硬件直接写入，接收端 CPU 无感知 */
            wr.opcode = IBV_WR_RDMA_WRITE;
        }
        
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = remote_info.addr + i * CHUNK_SIZE;
        wr.wr.rdma.rkey = remote_info.rkey;

        ibv_post_send(id->qp, &wr, &bad_wr);
        unpolled++;

        /* 滑动窗口维护：当未确认的 WR 达到 QP 深度或所有分块投递完毕时，批量回收 CQE */
        if (unpolled == QP_DEPTH || i == num_chunks - 1) {
            for (int j = 0; j < unpolled; j++) {
                poll_completion(id->send_cq);
            }
            unpolled = 0;
        }
    }

    gettimeofday(&t2, NULL);
    *cost = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;
    
    printf("[RDMA] Pipelined WRITE_WITH_IMM Complete! Sent %d small chunks.\n", num_chunks);

    /* 资源销毁与回收 */
    rdma_dereg_mr(info_mr); 
    rdma_dereg_mr(mr);
    rdma_disconnect(id); 
    rdma_destroy_ep(id);
    return 0;
}

/**
 * @brief       Slave 接收端：被动监听并接收 RDMA 内存写入
 * @param[in]   buffer 预先分配的本地内存基址，用于承接远程写入的数据
 * @param[in]   size   期望接收的总字节数
 * @param[out]  cost   传出参数，记录网络传输阶段的耗时 (ms)
 * @return      int    0: 接收成功; -1: 链路建立或接收失败
 * @note        接收端仅需注册内存并下发一个带有空 SGE 的 Receive Request。
 * 当发送端触发 WRITE_WITH_IMM 时，接收端将在 CQ 中捕获到该事件，完成同步。
 */
int kvs_rdma_recv_memory(void *buffer, size_t size, double *cost) {
    struct rdma_addrinfo hints, *res;
    struct rdma_cm_id *listen_id, *id;
    struct ibv_mr *mr, *info_mr, *dummy_mr;
    struct rdma_mem_info local_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = RAI_PASSIVE; 
    hints.ai_port_space = RDMA_PS_TCP;
    if (rdma_getaddrinfo(NULL, RDMA_PORT, &hints, &res)) return -1;

    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.cap.max_send_wr = 16; 
    attr.cap.max_recv_wr = 16;
    attr.cap.max_send_sge = 1; 
    attr.cap.max_recv_sge = 1;
    attr.sq_sig_all = 1;

    if (rdma_create_ep(&listen_id, res, NULL, &attr)) return -1;
    rdma_freeaddrinfo(res);
    
    if (rdma_listen(listen_id, 1)) return -1;
    if (rdma_get_request(listen_id, &id)) return -1;

    /* 独立计时：评估接收端内存注册 (MR) 造成的上下文开销 */
    struct timeval mr_t1, mr_t2;
    gettimeofday(&mr_t1, NULL);
    mr = rdma_reg_write(id, buffer, size);
    local_info.addr = (uintptr_t)buffer; 
    local_info.rkey = mr->rkey;
    info_mr = rdma_reg_msgs(id, &local_info, sizeof(local_info));
    gettimeofday(&mr_t2, NULL);
    printf("[RDMA] Memory Registration Overhead: %.2f ms (To be eliminated via Global MR Pool)\n", 
          (mr_t2.tv_sec - mr_t1.tv_sec) * 1000.0 + (mr_t2.tv_usec - mr_t1.tv_usec) / 1000.0);

    /* 构造 Dummy MR 与 Receive WR，用于捕获发送端的 Immediate Data */
    char dummy = '0';
    dummy_mr = rdma_reg_msgs(id, &dummy, 1);
    struct ibv_sge dsge = { 
        .addr = (uintptr_t)&dummy, 
        .length = 1, 
        .lkey = dummy_mr->lkey 
    };
    struct ibv_recv_wr recv_wr = {0}, *bad_recv_wr;
    
    recv_wr.wr_id = 999; 
    recv_wr.sg_list = &dsge; 
    recv_wr.num_sge = 1;
    
    if (ibv_post_recv(id->qp, &recv_wr, &bad_recv_wr)) return -1;
    if (rdma_accept(id, NULL)) return -1;

    /* 将本地物理内存映射凭证 (R_Key & Addr) 发送给对端 */
    rdma_post_send(id, NULL, &local_info, sizeof(local_info), info_mr, 0);
    poll_completion(id->send_cq);

    struct timeval t1, t2;
    gettimeofday(&t1, NULL);

    /* 阻塞等待对端携带 Immediate Data 的写完成事件到达，标志整体传输结束 */
    if (poll_completion(id->recv_cq) < 0) return -1;

    gettimeofday(&t2, NULL);
    *cost = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;

    /* 资源销毁与回收 */
    rdma_dereg_mr(dummy_mr); 
    rdma_dereg_mr(info_mr); 
    rdma_dereg_mr(mr);
    rdma_disconnect(id); 
    rdma_destroy_ep(id); 
    rdma_destroy_ep(listen_id);
    
    return 0;
}