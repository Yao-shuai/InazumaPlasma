/**
 * @file        xdp_prog.c
 * @brief       InazumaKV 内核态 AF_XDP 旁路网络拦截器 (eBPF 字节码)
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        该文件将被 Clang/LLVM 编译为 eBPF 字节码，并挂载于网卡驱动的 RX 早期路径 (XDP 钩子)。
 * 核心逻辑：执行 L2/L3/L4 协议头的高速解复用 (Demultiplexing)，
 * 识别目的端口为 8888 的 UDP 报文，并将其直接重定向至用户态的 AF_XDP 内存池 (UMEM)，
 * 从而彻底绕过 Linux 内核 TCP/IP 协议栈，实现纳秒级延迟的 Kernel Bypass。
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h> 

/* ==========================================================================
 * BPF 映射定义 (eBPF Maps)
 * ========================================================================== */

/**
 * @brief XSKMAP (AF_XDP Socket Map)
 * @note  用于在内核 XDP 程序与用户态 AF_XDP Socket 之间建立重定向通道。
 * 键 (Key) 通常为网卡的接收队列 ID (RX Queue Index)，
 * 值 (Value) 为对应队列上绑定的用户态 XDP Socket 描述符。
 */
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
} xsks_map SEC(".maps");


/* ==========================================================================
 * XDP 核心拦截过滤程序 (Core Packet Filter)
 * ========================================================================== */

/**
 * @brief       XDP 报文处理主函数
 * @param[in]   ctx XDP 元数据上下文，包含当前数据包在内存中的起始与终止地址
 * @return      int 返回 XDP 动作指令 (XDP_PASS, XDP_DROP, XDP_REDIRECT 等)
 */
SEC("xdp")
int xdp_sock_prog(struct xdp_md *ctx) {
    /* 获取报文的物理内存直接访问指针 */
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    /* 1. L2 链路层解析与安全校验 (Ethernet Header) */
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) {
        return XDP_PASS; 
    }

    /* 2. 协议过滤：放行非 IPv4 报文 (ETH_P_IP = 0x0800) */
    if (eth->h_proto != bpf_htons(ETH_P_IP)) {
        return XDP_PASS; 
    }

    /* 3. L3 网络层解析与安全校验 (IP Header) */
    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end) {
        return XDP_PASS;
    }

    /* 4. 协议过滤：放行非 UDP 报文 (IPPROTO_UDP = 17) */
    if (ip->protocol != IPPROTO_UDP) {
        return XDP_PASS; 
    }

    /* 5. 动态计算 IP 首部的实际长度 (ihl 字段单位为 4 字节 / 32-bit words) */
    __u32 ip_hdr_len = ip->ihl * 4;
    if (ip_hdr_len < sizeof(struct iphdr)) {
        return XDP_PASS; 
    }

    /*
     * [内核验证器约束/边界防御]
     * 核心安全要求：在将内存指针强制转换为 UDP 首部结构之前，必须向 eBPF 内核验证器
     * (Verifier) 提供极其明确的数学证明，保证 (IP首部长度 + UDP首部长度) 的内存访问
     * 绝对不会超出网卡 DMA 缓冲区的末尾 (data_end)，否则字节码将被内核拒绝加载。
     */
    if ((void *)ip + ip_hdr_len + sizeof(struct udphdr) > data_end) {
        return XDP_PASS;
    }

    /* 6. L4 传输层解析 (UDP Header) */
    struct udphdr *udp = (void *)ip + ip_hdr_len;

    /* 协议过滤：识别 InazumaKV 专属的高频增量同步端口 (8888) */
    if (udp->dest != bpf_htons(8888)) {
        return XDP_PASS; 
    }

    /*
     * 7. [优雅降级机制] 命中目标端口后，执行网卡队列至 AF_XDP 的重定向
     * @note 将 bpf_redirect_map 的 Flag 参数设定为 XDP_PASS 是关键的防黑洞设计。
     * 当用户态网关进程异常崩溃或未能成功绑定 XSKMAP 时，当前特定的 8888 端口报文
     * 将平滑地回退给内核协议栈继续处理，而不会被静默丢弃 (XDP_DROP)。
     */
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

/* ==========================================================================
 * 内核模块许可证 (License)
 * ========================================================================== */

/** @brief 声明开源许可证，允许此 eBPF 程序调用受 GPL 保护的内核辅助函数 (Helpers) */
char _license[] SEC("license") = "GPL";