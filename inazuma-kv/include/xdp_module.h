/**
 * @file        xdp_module.h
 * @brief       InazumaKV AF_XDP 旁路网络通信模块头文件
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        提供基于 AF_XDP (eXpress Data Path) 的底层网络 I/O 接口。
 * 支持 Master 模式 (增量指令封包与零拷贝发送) 与 Slave/Gateway 模式
 * (基于 NAPI 的死循环轮询接收)，实现绕过内核协议栈的纳秒级跨机通信。
 */

#ifndef __XDP_MODULE_H__
#define __XDP_MODULE_H__

/* ==========================================================================
 * 生命周期管理接口 (Lifecycle Management APIs)
 * ========================================================================== */

/**
 * @brief       初始化 AF_XDP 底层通信环境及 UMEM 内存池
 * @param[in]   ifname          绑定的底层网络接口名称 (如 "eth0", "ens33")
 * @param[in]   queue_id        绑定的网卡硬件接收/发送队列 ID (配合 CPU 绑核使用)
 * @param[in]   is_master       角色标识：非 0 表示 Master 端，0 表示 Slave 端
 * @param[in]   target_mac_str  目标 Slave 节点的物理 MAC 地址 (Master 模式必填，用于直构以太网帧头)
 * @param[in]   target_ip_str   目标 Slave 节点的 IPv4 地址 (Master 模式必填，用于直构 IP 报文头)
 * @return      int             0: 初始化成功; -1: 失败 (特权级不足、设备不支持或内存分配失败)
 * @note        该函数将加载底层的 eBPF 字节码至网卡驱动钩子，并分配/映射用于零拷贝
 * 数据交换的用户态内存块 (UMEM) 及环形队列 (Fill/Completion/Rx/Tx Rings)。
 */
int xdp_init(const char *ifname, int queue_id, int is_master, const char *target_mac_str, const char *target_ip_str);

/**
 * @brief       销毁 AF_XDP 通信上下文并释放内核与硬件资源
 * @note        卸载绑定的 eBPF 程序，关闭 XSK 套接字，并解除 UMEM 的 mmap 映射，
 * 确保网卡恢复至标准的内核协议栈处理模式。
 */
void xdp_cleanup();


/* ==========================================================================
 * 数据面收发接口 (Data Plane I/O APIs)
 * ========================================================================== */

/**
 * @brief       基于 AF_XDP 环形队列的零拷贝数据发送 (Master 模式专属)
 * @param[in]   cmd_data        待发送的增量同步负载数据指针
 * @param[in]   len             负载数据的有效字节长度
 * @note        该接口负责将负载数据封装入预置了 L2/L3/L4 协议头的 UMEM 帧中，
 * 并将其投递至 Tx Ring，由内核网卡驱动直接 DMA 发送，规避传统的 sk_buff 内存分配与拷贝开销。
 */
void xdp_send_sync(const char *cmd_data, int len);

/**
 * @brief       启动 AF_XDP 数据面轮询事件循环 (Slave/Gateway 模式专属)
 * @note        这是一个阻塞型的死循环 (Busy-Polling Hot Loop)。
 * 通过高频轮询 Rx Ring 以第一时间获取到达网卡硬件缓冲区的网络报文，
 * 彻底消除内核硬件中断 (Hard IRQ) 及软中断 (SoftIRQ) 带来的上下文切换延迟。
 */
void xdp_poll_loop();

#endif // __XDP_MODULE_H__