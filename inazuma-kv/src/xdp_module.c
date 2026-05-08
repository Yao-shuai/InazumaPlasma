/**
 * @file        xdp_module.c
 * @brief       AF_XDP 高性能独立网关核心实现 (Part 1: 基础架构与初始化)
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        提供绕过 Linux 内核协议栈的极致 I/O 吞吐。
 * 包含 UMEM 内存池分配、AF_XDP 套接字 (XSK) 绑定、eBPF 程序的加载与挂载，
 * 以及底层 UDP/IP/Ethernet 协议帧的物理内存构建算法。
 */

#define _GNU_SOURCE
#include "xdp_module.h"
#include "shm_ipc.h" 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <xdp/xsk.h>
#include <linux/if_link.h>
#include <sys/time.h>
#include <time.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

/* ==========================================================================
 * 宏定义与常量规约 (Macros & Constants)
 * ========================================================================== */

/** @brief 封装系统命令调用并抑制未使用返回值警告 */
#define RUN_SYSTEM(cmd) do { int ret = system(cmd); (void)ret; } while(0)

/** @brief UMEM 内存池划分的数据帧总数 (须为 2 的幂以优化对齐) */
#define NUM_FRAMES 524288
/** @brief 发送端与接收端乱序重排的滑动窗口容量 */
#define TX_WINDOW_SIZE 524288
/** @brief 单次从内核 Rx Ring 轮询提取的最大数据包数量 */
#define RX_BATCH_SIZE 256

/** @brief 单个 AF_XDP 数据帧的物理内存分配粒度 (标准 MTU 充足) */
#define FRAME_SIZE 2048
/** @brief 旁路网关专属的高频同步 UDP 端口 */
#define SYNC_PORT 8888
/** @brief 以太网标准要求的最小数据帧长度 (防 Padding 异常) */
#define ETH_MIN_LEN 60

/* 协议校验魔数 (Magic Numbers) */
#define XDP_TUNNEL_MAGIC 0x58445052  /**< 数据隧道报文魔数 ("XDPR") */
#define XDP_NACK_MAGIC   0x4E41434B  /**< 丢包重传请求魔数 ("NACK") */
#define XDP_ACK_MAGIC    0x41434B53  /**< 确认接收应答魔数 ("ACKS") */

/* ==========================================================================
 * 网络协议头与缓存窗口定义 (Protocol Headers & Window Caches)
 * ========================================================================== */

/**
 * @brief 自定义 UDP 负载头部 (数据透传模式)
 * @note  采用 __attribute__((packed)) 取消编译器字节对齐，严格适配网络字节序
 */
struct xdp_tunnel_hdr {
    uint32_t magic;      /**< 隧道标识魔数 */
    uint64_t seq;        /**< 全局单调递增的序列号 (用于乱序重排与 SACK) */
    uint32_t len;        /**< 负载数据真实长度 */
} __attribute__((packed));

/**
 * @brief 自定义 UDP 负载头部 (网络控制面模式 - ACK/NACK)
 */
struct xdp_ctrl_hdr {
    uint32_t magic;      /**< 控制帧标识魔数 */
    uint64_t seq;        /**< 期望的或已确认的序列号基准 */
} __attribute__((packed));

/**
 * @brief 数据包发送缓存单元 (用于丢包重传机制)
 */
struct tx_pkt_cache {
    uint64_t seq;             /**< 缓存包的序列号 */
    int len;                  /**< 缓存包有效载荷长度 */
    char data[MAX_PAYLOAD];   /**< 缓存包原始数据拷贝 */
};

/* ==========================================================================
 * 全局运行上下文与网卡映射描述符 (Global Contexts)
 * ========================================================================== */

/** @brief 进程间共享内存 IPC 句柄 */
struct shm_context *g_shm = NULL;

/** @brief 发送端历史数据滑动窗口基址 */
static struct tx_pkt_cache *g_tx_window = NULL;  
/** @brief 接收端乱序数据暂存窗口基址 (Out-of-Order Window) */
static struct tx_pkt_cache *g_ooo_window = NULL; 

/* 序列号状态机变量 */
static uint64_t g_master_tx_seq = 1;        /**< Master 发送基准游标 */
static uint64_t g_highest_ack_seq = 1;      /**< Master 已被确认的最高连续游标 */
static uint64_t g_slave_expected_seq = 1;   /**< Slave 期望接收的下一序列号 */
static uint64_t g_slave_highest_seq = 0;    /**< Slave 观测到的网络最高乱序序列号 */

/* AF_XDP 核心组件结构 */
static struct xsk_umem *umem;               /**< 用户态内存池描述符 */
static struct xsk_socket *xsk;              /**< AF_XDP 套接字描述符 */
static struct xsk_ring_prod fq;             /**< Fill Queue: 向内核提供空闲接收帧 */
static struct xsk_ring_cons cq;             /**< Completion Queue: 回收已发送的空闲帧 */
static struct xsk_ring_prod tx;             /**< Tx Queue: 提交待发送的业务帧 */
static struct xsk_ring_cons rx;             /**< Rx Queue: 提取已到达的业务帧 */
static void *umem_buffer;                   /**< UMEM 物理内存基址 */

/* 网络层寻址与路由拓扑变量 */
static int g_is_master = 0;
static uint8_t g_dest_mac[6];               /**< 目标网卡硬件 MAC 地址 */
static uint8_t g_src_mac[6];                /**< 宿主网卡硬件 MAC 地址 */
static uint32_t g_dest_ip;                  /**< 目标网络 IPv4 地址 (网络字节序) */
static uint32_t g_src_ip;                   /**< 宿主网络 IPv4 地址 (网络字节序) */
static char g_ifname[IFNAMSIZ];             /**< 绑定的网络接口名称 */
static uint32_t g_tx_frame_idx = 0;         /**< UMEM 帧分配游标 */

extern volatile int g_keep_running;

/* ==========================================================================
 * 基础网络工具与校验算法 (Network Utilities & Checksums)
 * ========================================================================== */

/**
 * @brief       计算标准 IP 头部的 16-bit 校验和
 * @param[in]   buf     指向待计算数据的起始地址
 * @param[in]   nwords  16-bit 字的数量
 * @return      unsigned short 补码计算后的校验和
 */
unsigned short checksum(unsigned short *buf, int nwords) {
    unsigned long sum = 0;
    for (; nwords > 0; nwords--) sum += *buf++;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (unsigned short)(~sum);
}

/**
 * @brief       基于 ioctl 获取指定网卡接口的 MAC 地址
 * @param[in]   ifname 网络接口名称
 * @param[out]  mac    承接 MAC 地址的 6 字节数组
 * @return      int    0: 成功; -1: 获取失败
 */
static int get_mac_address(const char *ifname, uint8_t *mac) {
    struct ifreq ifr; 
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    if (fd < 0) {
        return -1;
    }
    
    memset(&ifr, 0, sizeof(ifr)); 
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) { 
        close(fd); 
        return -1; 
    }
    
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6); 
    close(fd); 
    return 0;
}

/**
 * @brief 提取毫秒级时间戳的内联原语
 */
uint64_t get_ms() {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* ==========================================================================
 * AF_XDP 核心生命周期与 UMEM 初始化 (Lifecycle & UMEM Setup)
 * ========================================================================== */

/**
 * @brief       初始化 AF_XDP 环境，完成内存池与驱动层绑核挂载
 * @note        此阶段完成 eBPF 拦截程序的挂载、内存池分配及滑动窗口预置。
 */
int xdp_init(const char *ifname, int queue_id, int is_master, const char *target_mac_str, const char *target_ip_str) {
    g_is_master = is_master;
    strncpy(g_ifname, ifname, IFNAMSIZ - 1); 
    
    char cmd[256];
    printf("[%s] Preparing AF_XDP environment (Strict Precision Engine)...\n", is_master ? "XDP-Master" : "XDP-Slave");
    
    /* 卸载历史遗留的 eBPF 字节码，保证网卡驱动状态纯净 */
    snprintf(cmd, sizeof(cmd), "ip link set dev %s xdp off 2>/dev/null", ifname); RUN_SYSTEM(cmd);
    snprintf(cmd, sizeof(cmd), "ip link set dev %s xdpgeneric off 2>/dev/null", ifname); RUN_SYSTEM(cmd);

    /* 分配严格对齐的内存页以支撑 UMEM 零拷贝 */
    if (posix_memalign(&umem_buffer, getpagesize(), NUM_FRAMES * FRAME_SIZE)) return -1;
    if (xsk_umem__create(&umem, umem_buffer, NUM_FRAMES * FRAME_SIZE, &fq, &cq, NULL)) return -1;
    
    /* 预分配发送与接收的乱序滑动窗口 */
    g_tx_window = calloc(TX_WINDOW_SIZE, sizeof(struct tx_pkt_cache));
    g_ooo_window = calloc(TX_WINDOW_SIZE, sizeof(struct tx_pkt_cache));

    /* 绑定 AF_XDP 零拷贝核心通信 Socket */
    struct xsk_socket_config cfg;
    memset(&cfg, 0, sizeof(cfg)); 
    cfg.rx_size = 2048; 
    cfg.tx_size = 2048; 
    cfg.libxdp_flags = 1; 
    cfg.bind_flags = XDP_COPY; 

    if (xsk_socket__create(&xsk, ifname, queue_id, umem, &rx, &tx, &cfg)) return -1;

    /* 加载并注入 eBPF 内核旁路拦截字节码 */
    struct bpf_object *bpf_obj = bpf_object__open_file("bin/xdp_prog.o", NULL);
    if (!bpf_obj || bpf_object__load(bpf_obj)) return -1;
    
    struct bpf_program *bpf_prog = bpf_object__find_program_by_name(bpf_obj, "xdp_sock_prog");
    if (!bpf_prog) return -1;
    
    int prog_fd = bpf_program__fd(bpf_prog);
    int ifindex = if_nametoindex(ifname);
    
    /* 优先尝试最高速的 DRV_MODE 驱动卸载模式，失败则回退至 SKB_MODE */
    if (bpf_set_link_xdp_fd(ifindex, prog_fd, XDP_FLAGS_DRV_MODE) < 0) {
        if (bpf_set_link_xdp_fd(ifindex, prog_fd, XDP_FLAGS_SKB_MODE) < 0) return -1;
    }
    
    /* 更新 eBPF Map，将网卡 Rx 队列与用户态 XSK 描述符绑定映射 */
    int map_fd = bpf_object__find_map_fd_by_name(bpf_obj, "xsks_map");
    int xsk_fd = xsk_socket__fd(xsk);
    if (bpf_map_update_elem(map_fd, &queue_id, &xsk_fd, BPF_ANY)) return -1;

    /* 解析网络硬件路由寻址信息 */
    get_mac_address(ifname, g_src_mac);
    g_src_ip = inet_addr(is_master ? "192.168.124.13" : "192.168.124.14");

    if (g_is_master) {
        if (target_mac_str) {
            struct ether_addr *addr = ether_aton(target_mac_str);
            if (addr) memcpy(g_dest_mac, addr->ether_addr_octet, 6);
        }
        if (target_ip_str) g_dest_ip = inet_addr(target_ip_str);
    } else {
        struct ether_addr *addr = ether_aton("00:0c:29:de:bc:bf");
        if (addr) memcpy(g_dest_mac, addr->ether_addr_octet, 6);
        g_dest_ip = inet_addr("192.168.124.13");
    }

    /* 预填充底层硬件的 Rx 接收可用队列 (Fill Queue) */
    uint32_t idx = 0;
    unsigned int reserved = xsk_ring_prod__reserve(&fq, 2048, &idx);
    for (int i = 0; i < reserved; i++) *xsk_ring_prod__fill_addr(&fq, idx++) = i * FRAME_SIZE;
    xsk_ring_prod__submit(&fq, reserved);
    
    printf("[Gateway] RX Pool & 1GB Sliding Windows allocated successfully.\n");
    return 0;
}

/**
 * @brief 清理并释放全部 AF_XDP 资源及内存映射
 */
void xdp_cleanup() {
    if (xsk) xsk_socket__delete(xsk);
    if (umem) xsk_umem__delete(umem);
    if (umem_buffer) free(umem_buffer);
    if (g_tx_window) free(g_tx_window);
    if (g_ooo_window) free(g_ooo_window);
}

/* ==========================================================================
 * 数据帧重构与发送准备 (Frame Reconstruction & Tx Preparation)
 * ========================================================================== */

/**
 * @brief       在连续物理内存上直接封装 L2/L3/L4 协议头
 * @param[in]   pkt           UMEM 帧的物理首址
 * @param[in]   payload_data  待封装的负载数据指针
 * @param[in]   payload_len   待封装的负载长度
 * @param[in]   is_ctrl       标识是否为控制帧 (ACK/NACK)，将改变目标端口映射
 */
static void build_udp_frame(char *pkt, const char *payload_data, int payload_len, int is_ctrl) {
    struct ethhdr *eth = (struct ethhdr *)pkt;
    struct iphdr *ip = (struct iphdr *)(pkt + sizeof(struct ethhdr));
    struct udphdr *udp = (struct udphdr *)(pkt + sizeof(struct ethhdr) + sizeof(struct iphdr));
    char *payload = (char *)(udp + 1);

    memcpy(payload, payload_data, payload_len);
    
    /* 构建 L2 以太网头 */
    memcpy(eth->h_dest, g_dest_mac, 6); 
    memcpy(eth->h_source, g_src_mac, 6); 
    eth->h_proto = htons(ETH_P_IP);

    /* 构建 L3 IPv4 头 */
    ip->ihl = 5; ip->version = 4; ip->tos = 0; ip->ttl = 64; ip->protocol = IPPROTO_UDP;
    ip->saddr = g_src_ip; ip->daddr = g_dest_ip;
    ip->tot_len = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + payload_len); 
    ip->check = 0; 
    ip->check = checksum((unsigned short *)ip, sizeof(struct iphdr) / 2);

    /* 构建 L4 UDP 头 */
    udp->source = htons(is_ctrl ? 8888 : 12345); 
    udp->dest = htons(SYNC_PORT);
    udp->len = htons(sizeof(struct udphdr) + payload_len); 
    udp->check = 0;
}

/**
 * @brief 获取下一可用 UMEM 发送帧地址偏移，跳过预留给 Rx 的前 4096 个槽位
 */
uint64_t get_next_tx_addr() {
    uint64_t addr = (4096 + (g_tx_frame_idx % (NUM_FRAMES - 4096))) * FRAME_SIZE;
    g_tx_frame_idx++;
    return addr;
}

/**
 * @brief       处理完成队列 (CQ) 以快速回收已发送的内核物理帧描述符
 * @note        采用 512 的批量步进回收策略，极大地缩减了内核/用户态状态同步开销。
 */
static inline int process_tx_cq() {
    uint32_t idx_cq; unsigned int completed;
    completed = xsk_ring_cons__peek(&cq, 512, &idx_cq);
    if (completed > 0) {
        xsk_ring_cons__release(&cq, completed);
    }
    return completed;
}

/**
 * @brief       组装网络控制协议帧 (ACK / NACK) 并挂入 Tx 环形队列
 * @param[in]   idx_tx  Tx 环中的目标槽位索引
 * @param[in]   magic   控制命令类型魔数
 * @param[in]   seq     状态机确认/请求的游标位
 */
void xdp_prepare_ctrl_msg(uint32_t idx_tx, uint32_t magic, uint64_t seq) {
    uint64_t addr = get_next_tx_addr();
    char *pkt = (char*)xsk_umem__get_data(umem_buffer, addr);
    
    struct xdp_ctrl_hdr ctrl; 
    ctrl.magic = htonl(magic); 
    ctrl.seq = seq;
    
    build_udp_frame(pkt, (char*)&ctrl, sizeof(ctrl), 1);
    
    struct xdp_desc *desc = xsk_ring_prod__tx_desc(&tx, idx_tx);
    desc->addr = addr; 
    desc->len = sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + sizeof(ctrl);
    if (desc->len < ETH_MIN_LEN) desc->len = ETH_MIN_LEN;
}

/**
 * @brief       组装业务数据隧道帧并缓存后挂入 Tx 环形队列
 * @param[in]   idx           Tx 环中的目标槽位索引
 * @param[in]   cmd_data      需透传的应用层报文
 * @param[in]   cmd_len       应用层报文物理长度
 * @param[in]   seq           当前指令挂载的网络序列号
 * @param[in]   is_retransmit 标识当前操作是否为丢包重传 (规避窗口重复录入)
 */
void xdp_prepare_tx_desc(uint32_t idx, const char *cmd_data, int cmd_len, uint64_t seq, int is_retransmit) {
    uint64_t addr = get_next_tx_addr();
    char *pkt = (char*)xsk_umem__get_data(umem_buffer, addr);
    
    char buf[2048];
    struct xdp_tunnel_hdr *hdr = (struct xdp_tunnel_hdr *)buf;
    hdr->magic = htonl(XDP_TUNNEL_MAGIC); 
    hdr->seq = seq; 
    hdr->len = htonl(cmd_len);
    memcpy(buf + sizeof(struct xdp_tunnel_hdr), cmd_data, cmd_len);

    int total_len = sizeof(struct xdp_tunnel_hdr) + cmd_len;
    build_udp_frame(pkt, buf, total_len, 0);

    /* 初次下发时录入历史滑动窗口以支撑后续的 SACK 或超时重传请求 */
    if (!is_retransmit) {
        int win_idx = seq % TX_WINDOW_SIZE;
        g_tx_window[win_idx].seq = seq; 
        g_tx_window[win_idx].len = cmd_len;
        memcpy(g_tx_window[win_idx].data, cmd_data, cmd_len);
        g_master_tx_seq++;
    }

    struct xdp_desc *desc = xsk_ring_prod__tx_desc(&tx, idx);
    desc->addr = addr; 
    desc->len = sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + total_len;
    
    if (desc->len < ETH_MIN_LEN) desc->len = ETH_MIN_LEN;
}

/**
 * @file        xdp_module.c
 * @brief       AF_XDP 高性能独立网关核心实现 (Part 2: XDP 主事件循环与 AIMD 拥塞控制)
 * @author      Nexus_Yao (or Your Name)
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        实现网关的非阻塞轮询状态机 (Polling State Machine)。
 * 深度集成了应用层的 AIMD 拥塞控制协议、乱序缓存窗口 (OOO Window)、
 * 精准 SACK 重传机制，以及适配虚拟化环境的超线程调度让出 (Hypervisor Yielding)。
 */

/* ==========================================================================
 * AF_XDP 非阻塞轮询主事件循环 (Main Polling Event Loop)
 * ========================================================================== */

/**
 * @brief       启动旁路网关的数据面死循环
 * @note        此函数将独占当前 CPU 核心。通过极高频的用户态轮询，
 * 直接操作网卡的 DMA 环形队列，实现真正意义上的 Kernel Bypass。
 */
void xdp_poll_loop() {
    printf("====================================================\n");
    printf(" [Gateway] AIMD Congestion Control & VM-Yield Enabled...\n");
    printf("====================================================\n");

    /* 挂载到主引擎的共享内存通信通道 */
    g_shm = shm_ipc_init(false);
    if (!g_shm) {
        printf("[Fatal] Failed to attach to SHM! Ensure KV Engine is running first.\n");
        return;
    }

    char shm_payload[MAX_PAYLOAD];
    uint32_t shm_payload_len;
    
    /* 记录当前积压在 Tx Ring 尚未调用 sendto 触发网卡发送的帧数 */
    int tx_pending = 0;

    /* 状态机时间轴与序列号记录 */
    static uint64_t last_ack_time = 0;
    static uint64_t last_nack_time = 0;
    static uint64_t last_nack_seq = 0;
    static uint64_t last_tx_activity = 0; 
    static uint32_t unacked_rx_pkts = 0; 
    
    /* * 拥塞控制核心变量：AIMD (Additive Increase Multiplicative Decrease)
     * 初始窗口设为 128 (试探性慢启动)。最大物理约束阈值为 4096。
     */
    static uint32_t cwnd = 128; 
    const uint32_t MAX_CWND = 4096; 
    
    /* 空闲自旋计数器，用于触发 VM-Exit 睡眠让出物理核心 */
    static int idle_spins = 0; 

    /* 性能遥测统计计数器 */
    unsigned long loop_counter = 0;
    unsigned long total_pkts = 0;
    unsigned long last_pkts = 0;
    time_t last_time = time(NULL);

    while (g_keep_running) {
        int work_done = 0;
        uint64_t now_ms = get_ms();

        /* 每历经 1,048,576 次指令周期，执行一次吞吐量采样核算与输出 */
        if (++loop_counter >= 1048576) {
            loop_counter = 0; 
            time_t now_time = time(NULL);
            if (now_time != last_time) {
                if (g_is_master) {
                    printf("[Gateway] Throughput: %lu pkts/sec | CWND: %u\n", total_pkts - last_pkts, cwnd);
                } else {
                    printf("[Gateway] Throughput: %lu pkts/sec\n", total_pkts - last_pkts);
                }
                fflush(stdout); 
                last_pkts = total_pkts; 
                last_time = now_time;
            }
        }

        /* ==========================================================================
         * 优先级 1: Master 端发包流控与指令下发 (Tx Dispatch & Flow Control)
         * ========================================================================== */
        if (g_is_master && g_shm) {
            int batch_tx = 0;
            
            /* 严格服从动态 CWND 约束：在途未确认的数据帧总量不可超越当前拥塞窗口 */
            while (batch_tx < 512 && (g_master_tx_seq - g_highest_ack_seq < cwnd)) {
                if (spsc_dequeue(&g_shm->tx_queue, shm_payload, &shm_payload_len)) {
                    uint32_t idx_tx = 0;
                    int reserve_spins = 0; 
                    
                    /* 尝试在 Tx Ring 中预留物理帧槽位 */
                    while (xsk_ring_prod__reserve(&tx, 1, &idx_tx) < 1) {
                        /* 延迟发送优化：Tx 环满时不再高频敲击系统调用，每 128 次自旋执行一次 flush */
                        if (++reserve_spins > 128) {
                            sendto(xsk_socket__fd(xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
                            reserve_spins = 0;
                        }
                        process_tx_cq(); 
                        __builtin_ia32_pause(); /* 缓解超线程资源争用 */
                    }
                    
                    xdp_prepare_tx_desc(idx_tx, shm_payload, shm_payload_len, g_master_tx_seq, 0);
                    xsk_ring_prod__submit(&tx, 1);
                    tx_pending++; 
                    batch_tx++; 
                    work_done++;
                    total_pkts++; 
                    last_tx_activity = now_ms;
                } else {
                    break; /* IPC 共享内存无待发数据 */
                }
            }

            /* * RTO (Retransmission TimeOut) 兜底：
             * 发送窗口内出现严重超时（10ms 缺乏有效响应），执行严厉的乘性减窗（惩罚），
             * 并从已知的最高连续 ACK 游标处启动强力补发。
             */
            if (g_master_tx_seq > g_highest_ack_seq && (now_ms - last_tx_activity > 10)) {
                
                cwnd = (cwnd / 2 > 32) ? (cwnd / 2) : 32; 

                uint64_t resend_start = g_highest_ack_seq;
                uint64_t pkts_to_resend = g_master_tx_seq - resend_start;
                if (pkts_to_resend > 512) pkts_to_resend = 512; 
                
                for (int r = 0; r < pkts_to_resend; r++) {
                    uint64_t resend_seq = resend_start + r;
                    int win_idx = resend_seq % TX_WINDOW_SIZE;
                    
                    /* 校验历史滑动窗口中的有效性并执行重新注入 */
                    if (g_tx_window[win_idx].seq == resend_seq) {
                        uint32_t resend_idx_tx = 0;
                        if (xsk_ring_prod__reserve(&tx, 1, &resend_idx_tx) >= 1) {
                            xdp_prepare_tx_desc(resend_idx_tx, g_tx_window[win_idx].data, g_tx_window[win_idx].len, resend_seq, 1);
                            xsk_ring_prod__submit(&tx, 1);
                            tx_pending++; 
                            total_pkts++; 
                        }
                    }
                }
                last_tx_activity = now_ms; 
            }
        }

        /* ==========================================================================
         * 优先级 2: 处理网卡底层 Rx 队列接收事件 (Rx Fetching & Demuxing)
         * ========================================================================== */
        uint32_t idx_rx = 0;
        unsigned int rcvd = xsk_ring_cons__peek(&rx, 512, &idx_rx);
        if (rcvd > 0) {
            uint32_t idx_fq = 0;
            /* 预先从 Fill Queue 中请求补充等量的空闲物理帧用于置换 */
            unsigned int ret_fq = xsk_ring_prod__reserve(&fq, rcvd, &idx_fq);
            unsigned int to_process = (ret_fq < rcvd) ? ret_fq : rcvd;

            if (to_process > 0) {
                for (unsigned int i = 0; i < to_process; i++) {
                    uint64_t addr = xsk_ring_cons__rx_desc(&rx, idx_rx + i)->addr;
                    /* 置换操作：将刚消费完的物理块地址重填回 FQ，供驱动下一轮使用 */
                    *xsk_ring_prod__fill_addr(&fq, idx_fq++) = addr; 

                    char *pkt = (char*)xsk_umem__get_data(umem_buffer, addr);
                    struct ethhdr *eth = (struct ethhdr *)pkt;
                    
                    /* 解析 L2 VLAN Tag / L3 IP / L4 UDP */
                    int offset = sizeof(struct ethhdr); 
                    if (eth->h_proto == htons(0x8100)) offset += 4; 
                    struct iphdr *ip = (struct iphdr *)(pkt + offset);
                    if (ip->protocol != IPPROTO_UDP) continue;
                    struct udphdr *udp = (struct udphdr *)(pkt + offset + ip->ihl * 4);
                    
                    /* 严苛端口过滤：拒绝一切非 8888 端口通讯 */
                    if (ntohs(udp->dest) != 8888) continue;

                    int payload_len = ntohs(udp->len) - 8; 
                    char *raw_payload = (char *)(udp + 1);

                    if (payload_len >= 4) {
                        uint32_t magic = ntohl(*(uint32_t *)raw_payload);
                        
                        /* ----------------------------------------------------------
                         * 角色：Master 处理对端的 ACK/NACK 反馈控制帧
                         * ---------------------------------------------------------- */
                        if (g_is_master && payload_len == sizeof(struct xdp_ctrl_hdr)) {
                            struct xdp_ctrl_hdr *ctrl = (struct xdp_ctrl_hdr *)raw_payload;
                            uint64_t seq = ctrl->seq;
                            
                            if (magic == XDP_ACK_MAGIC) {
                                /* 加性增窗：收到有序确认，平滑扩张拥塞窗口，提高吞吐上限 */
                                if (seq > g_highest_ack_seq) {
                                    g_highest_ack_seq = seq; 
                                    last_tx_activity = now_ms; 
                                    if (cwnd < MAX_CWND) cwnd += 1;
                                }
                            }
                            else if (magic == XDP_NACK_MAGIC) {
                                /* 乘性减窗：收到特定丢包声明，即刻削减窗口以缓解微突发过载 */
                                cwnd = (cwnd / 2 > 32) ? (cwnd / 2) : 32;

                                /* * 精准重传：模拟 SACK，不再全量回退。
                                 * 仅提取历史滑动窗口中对方指明的缺失 seq 帧重新投递。
                                 */
                                uint64_t resend_seq = seq; 
                                int win_idx = resend_seq % TX_WINDOW_SIZE;
                                if (g_tx_window[win_idx].seq == resend_seq) {
                                    uint32_t resend_idx_tx = 0;
                                    int reserve_spins = 0;
                                    while (xsk_ring_prod__reserve(&tx, 1, &resend_idx_tx) < 1) {
                                        if (++reserve_spins > 64) {
                                            sendto(xsk_socket__fd(xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
                                            reserve_spins = 0;
                                        }
                                        process_tx_cq(); 
                                        __builtin_ia32_pause();
                                    }
                                    xdp_prepare_tx_desc(resend_idx_tx, g_tx_window[win_idx].data, g_tx_window[win_idx].len, resend_seq, 1);
                                    xsk_ring_prod__submit(&tx, 1);
                                    tx_pending++; 
                                    total_pkts++;
                                }
                            }
                        }
                        /* ----------------------------------------------------------
                         * 角色：Slave 处理全量/增量业务数据透传帧
                         * ---------------------------------------------------------- */
                        else if (!g_is_master && magic == XDP_TUNNEL_MAGIC && payload_len >= sizeof(struct xdp_tunnel_hdr)) {
                            total_pkts++; 
                            struct xdp_tunnel_hdr *hdr = (struct xdp_tunnel_hdr *)raw_payload;
                            uint64_t pkt_seq = hdr->seq;
                            uint32_t real_cmd_len = ntohl(hdr->len);
                            char *real_cmd_data = raw_payload + sizeof(struct xdp_tunnel_hdr);
                            
                            if (pkt_seq > g_slave_highest_seq) g_slave_highest_seq = pkt_seq;

                            /* 情形 A：严格有序到达，符合期望 */
                            if (pkt_seq == g_slave_expected_seq) {
                                int enqueue_ok = 1;
                                if (g_shm) {
                                    if (!spsc_enqueue(&g_shm->rx_queue, real_cmd_data, real_cmd_len)) enqueue_ok = 0; 
                                }
                                
                                if (enqueue_ok) {
                                    g_slave_expected_seq++;
                                    unacked_rx_pkts++; 
                                    
                                    /* * 乱序恢复机制：一旦缺口被填平，
                                     * 立即拉链式递归检查并消费留存在 OOO 窗口内的已到达报文。
                                     */
                                    while (1) {
                                        int ooo_idx = g_slave_expected_seq % TX_WINDOW_SIZE;
                                        if (g_ooo_window[ooo_idx].seq == g_slave_expected_seq) {
                                            if (g_shm) {
                                                if (!spsc_enqueue(&g_shm->rx_queue, g_ooo_window[ooo_idx].data, g_ooo_window[ooo_idx].len)) break; 
                                            }
                                            g_ooo_window[ooo_idx].seq = 0; 
                                            g_slave_expected_seq++;
                                        } else break; 
                                    }
                                }
                            } 
                            /* 情形 B：序列号跃进（判定为发生链路丢包或重排） */
                            else if (pkt_seq > g_slave_expected_seq) {
                                /* 不丢弃，提前妥投至 OOO 缓存窗口 */
                                int ooo_idx = pkt_seq % TX_WINDOW_SIZE;
                                if (g_ooo_window[ooo_idx].seq != pkt_seq) { 
                                    g_ooo_window[ooo_idx].seq = pkt_seq;
                                    g_ooo_window[ooo_idx].len = real_cmd_len;
                                    memcpy(g_ooo_window[ooo_idx].data, real_cmd_data, real_cmd_len);
                                }
                                
                                /* 反压限流：为防止 NACK 风暴，1ms 物理时间刻度内最多声明一次重传 */
                                if (g_slave_expected_seq != last_nack_seq || (now_ms - last_nack_time >= 1)) {
                                    uint32_t idx_tx = 0;
                                    int reserve_spins = 0;
                                    while (xsk_ring_prod__reserve(&tx, 1, &idx_tx) < 1) {
                                        if (++reserve_spins > 64) {
                                            sendto(xsk_socket__fd(xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
                                            reserve_spins = 0;
                                        }
                                        process_tx_cq(); 
                                        __builtin_ia32_pause();
                                    }
                                    xdp_prepare_ctrl_msg(idx_tx, XDP_NACK_MAGIC, g_slave_expected_seq);
                                    xsk_ring_prod__submit(&tx, 1);
                                    tx_pending++;
                                    last_nack_seq = g_slave_expected_seq;
                                    last_nack_time = now_ms;
                                }
                            }
                            /* 情形 C：序列号倒退（收到早已在 OOO 窗口中消费或超时的历史包） */
                            else if (pkt_seq < g_slave_expected_seq) {
                                /* 此时对方可能刚刚脱离丢包态导致重传盲目，立刻补发当前期望基准纠正对方状态机 */
                                uint32_t idx_tx = 0;
                                if (xsk_ring_prod__reserve(&tx, 1, &idx_tx) >= 1) {
                                    xdp_prepare_ctrl_msg(idx_tx, XDP_ACK_MAGIC, g_slave_expected_seq);
                                    xsk_ring_prod__submit(&tx, 1);
                                    tx_pending++;
                                }
                            }
                        }
                    }
                }
                xsk_ring_prod__submit(&fq, to_process);
                xsk_ring_cons__release(&rx, to_process);
                work_done++;
            }
        }

        /* ==========================================================================
         * 优先级 3: Slave 定期/定流触发确认反馈 (ACK Batching & Heartbeat)
         * ========================================================================== */
        if (!g_is_master && (now_ms - last_ack_time >= 1 || unacked_rx_pkts >= 128)) {
            uint32_t idx_tx = 0;
            int reserve_spins = 0;
            while (xsk_ring_prod__reserve(&tx, 1, &idx_tx) < 1) {
                if (++reserve_spins > 64) {
                    sendto(xsk_socket__fd(xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
                    reserve_spins = 0;
                }
                process_tx_cq(); 
                __builtin_ia32_pause();
            }
            xdp_prepare_ctrl_msg(idx_tx, XDP_ACK_MAGIC, g_slave_expected_seq);
            xsk_ring_prod__submit(&tx, 1);
            tx_pending++;
            
            last_ack_time = now_ms;
            unacked_rx_pkts = 0; 
        }

        /* ==========================================================================
         * 优先级 4: CQ 回收与 I/O 驱动激活
         * ========================================================================== */
        if (process_tx_cq() > 0) work_done++;

        /* 将所有压入 Tx 队列的准备帧批量系统调用触发网卡发送 */
        if (tx_pending > 0) { 
            if (sendto(xsk_socket__fd(xsk), NULL, 0, MSG_DONTWAIT, NULL, 0) >= 0) tx_pending = 0; 
        }
        
        /* ==========================================================================
         * 架构感知调度: 应对虚拟机环境的 vCPU 退让 (Hypervisor Yielding)
         * ========================================================================== */
        if (!work_done) {
            idle_spins++;
            /* * 容错阈值：如果当前事件循环连续 1000 周期空转，判定为网络底层 (如 vSwitch) 
             * 缓冲区干涸。通过调用 usleep(1) 强制发起主动 VM-Exit 睡眠调用，
             * 将物理 CPU 的算力还回给宿主机的网卡中断线程。
             */
            if (idle_spins > 1000) {
                usleep(1); 
                idle_spins = 0;
            } else {
                __builtin_ia32_pause(); 
            }
        } else {
            idle_spins = 0; 
        }
    }
}
