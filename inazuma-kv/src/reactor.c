/**
 * @file        reactor.c
 * @brief       高性能 Epoll 网络反应堆 (整合 Direct Write 与 XDP 极速嗅探)
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        整个 InazumaKV 的异步事件循环核心。
 * 基于单线程 Epoll 模型构建，深度融合了 TCP_NODELAY、Direct Write (直接回写优化)、
 * 以及通过 1ms 微时隙轮询 eBPF/XDP 共享内存 (Kernel Bypass) 的高级网络栈技术。
 */

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <stdlib.h>
#include <time.h>      
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "kvstore.h"
#include "kvs_config.h"
#include "kvs_persist.h" 

/* ==========================================================================
 * 宏定义与网络连接上下文 (Macros & Connection Context)
 * ========================================================================== */

#define CONNECTION_SIZE 1024 
#define BUFFER_LENGTH   (1024 * 1024) 

/**
 * @brief 表示单个 TCP 连接的上下文状态与缓冲区
 */
struct conn {
    int fd;
    char *rbuffer; 
    int rlength;                  
    char *wbuffer; 
    int wlength;                  
};

/* 全局句柄与状态 */
int epfd = 0;
struct conn conn_list[CONNECTION_SIZE];
static msg_handler kvs_handler;

/* ==========================================================================
 * 外部子系统钩子声明 (External Sub-system Hooks)
 * ========================================================================== */

extern void kvs_aof_flush_to_kernel(void);
extern void kvs_shm_rx_poll(void);
extern void xdp_flush_to_slave(void);

/* ==========================================================================
 * 内部状态机回调与网络工具 (State Machine Callbacks & Network Utils)
 * ========================================================================== */

/* 前向声明 */
int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

/**
 * @brief       将给定的文件描述符设置为非阻塞模式 (O_NONBLOCK)
 * @param[in]   fd  目标文件描述符
 */
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief       初始化连接的接收/发送缓冲区 (内存懒加载)
 * @param[in]   fd  连接的文件描述符 (直接作为数组索引)
 * @note        避免系统启动时预分配大量闲置内存，仅在连接建立时分配。
 */
void init_conn_buffer(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;
    
    if (conn_list[fd].rbuffer == NULL) {
        conn_list[fd].rbuffer = (char *)malloc(BUFFER_LENGTH);
    }
    if (conn_list[fd].wbuffer == NULL) {
        conn_list[fd].wbuffer = (char *)malloc(BUFFER_LENGTH);
    }

    conn_list[fd].rlength = 0;
    conn_list[fd].wlength = 0;
    conn_list[fd].fd = fd;
}

/**
 * @brief       注册或修改 Epoll 监控事件
 */
int set_event(int fd, int event, int is_add) {
    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    if (is_add) {
        return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    } else {
        return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }
}

/**
 * @brief       新连接注册入口
 */
void event_register(int fd, int event) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;
    init_conn_buffer(fd);
    set_event(fd, EPOLLIN, 1);
}

/**
 * @brief       处理新客户端的建立连接 (Accept) 请求
 * @return      int 0: 成功; -1: 失败或超载
 * @note        在接受连接后，强制关闭 Nagle 算法 (TCP_NODELAY)，
 * 确保小体积的 KV 响应报文能瞬间发出，消除 40ms 的内核合并延迟。
 */
int accept_cb(int fd) {
    struct sockaddr_in clientaddr;
    socklen_t len = sizeof(clientaddr);

    int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
    if (clientfd < 0) return -1;

    if (clientfd >= CONNECTION_SIZE) {
        close(clientfd);
        return -1;
    }

    set_nonblocking(clientfd);

    /* 架构优化：禁用 Nagle 算法，彻底消除 Pipeline 下的网络延迟抖动 */
    int nodelay = 1;
    setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    event_register(clientfd, EPOLLIN);
    return 0;
}

/**
 * @brief       关闭连接并安全回收内存与 Epoll 监控点
 */
void close_conn(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;
    
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    
    if (conn_list[fd].rbuffer) {
        free(conn_list[fd].rbuffer);
        conn_list[fd].rbuffer = NULL;
    }
    if (conn_list[fd].wbuffer) {
        free(conn_list[fd].wbuffer);
        conn_list[fd].wbuffer = NULL;
    }
    
    close(fd);
    memset(&conn_list[fd], 0, sizeof(struct conn));
}

/* ==========================================================================
 * 数据面：读写回调执行逻辑 (Data Plane: Read/Write Callbacks)
 * ========================================================================== */

/**
 * @brief       处理已就绪的客户端读取事件 (EPOLLIN)
 * @param[in]   fd 触发事件的文件描述符
 * @return      int 实际读取的字节数，或 -1 失败
 * @note        包含大厂级 Direct Write (直接回写) 优化策略。
 * 在绝大多数小包响应场景下，直接调用 send 返回数据，
 * 避免了 epoll_ctl(EPOLL_CTL_MOD) 带来的极其昂贵的内核上下文切换。
 */
int recv_cb(int fd) {
    struct conn *c = &conn_list[fd];
    int remain_space = BUFFER_LENGTH - c->rlength;

    if (remain_space <= 0) {
        c->rlength = 0; 
        remain_space = BUFFER_LENGTH;
    }

    int count = recv(fd, c->rbuffer + c->rlength, remain_space, 0);
    if (count <= 0) {
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        close_conn(fd); 
        return -1;
    }

    c->rlength += count;

    /* 交由上层协议解析器 (RESP parser) 提取命令并执行引擎逻辑 */
    int processed = 0;
    c->wlength = kvs_handler(fd, c->rbuffer, c->rlength, c->wbuffer, &processed);

    /* 处理可能存在的粘包/半包：将未处理完的数据平移至缓冲区头部 */
    if (processed > 0) {
        if (c->rlength > processed) {
            int remain_data = c->rlength - processed;
            memmove(c->rbuffer, c->rbuffer + processed, remain_data);
            c->rlength = remain_data;
        } else {
            c->rlength = 0;
        }
    }

    /* 架构优化：Direct Write 机制 (直接系统调用回写) */
    if (c->wlength > 0) {
        int n = send(fd, c->wbuffer, c->wlength, 0);
        if (n >= 0) {
            if (n == c->wlength) {
                /* 数据完美发送完毕，无需注册 EPOLLOUT */
                c->wlength = 0; 
            } else {
                /* TCP 发送窗口满溢，剩余数据挂载 EPOLLOUT 异步等待 */
                int remain = c->wlength - n;
                memmove(c->wbuffer, c->wbuffer + n, remain);
                c->wlength = remain;
                set_event(fd, EPOLLOUT, 0); 
            }
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 窗口完全阻塞，挂载 EPOLLOUT 等待内核唤醒 */
            set_event(fd, EPOLLOUT, 0);
        } else {
            close_conn(fd);
            return -1;
        }
    }
    
    return count;
}

/**
 * @brief       处理已就绪的客户端异步发送事件 (EPOLLOUT)
 * @note        仅当 Direct Write 遇到发送窗口满溢时，才会被内核回调。
 * 成功清空缓冲区后，将降级切回 EPOLLIN 监听状态。
 */
int send_cb(int fd) {
    struct conn *c = &conn_list[fd];
    if (c->wlength > 0) {
        int n = send(fd, c->wbuffer, c->wlength, 0);
        if (n < 0) {
            if (errno == EAGAIN) return 0;
            close_conn(fd);
            return -1;
        }
        
        if (n == c->wlength) {
            c->wlength = 0;
            set_event(fd, EPOLLIN, 0); /* 彻底排空，切回读监听 */
        } else {
            int remain = c->wlength - n;
            memmove(c->wbuffer, c->wbuffer + n, remain);
            c->wlength = remain;
        }
    } else {
        set_event(fd, EPOLLIN, 0);
    }
    return 0;
}

/* ==========================================================================
 * 服务器核心运行层 (Server Initialization & Main Loop)
 * ========================================================================== */

/**
 * @brief       初始化底层 TCP 监听 Socket
 */
int r_init_server(unsigned short port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    
    /* 动态读取配置参数，支持网卡绑定实现物理网络隔离 */
    if (strlen(g_config.bind_ip) > 0 && strcmp(g_config.bind_ip, "0.0.0.0") != 0) {
        servaddr.sin_addr.s_addr = inet_addr(g_config.bind_ip);
        printf("[Reactor] Strictly binding to IP: %s\n", g_config.bind_ip);
    } else {
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        printf("[Reactor] Binding to all interfaces (0.0.0.0)\n");
    }

    servaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed");
        return -1;
    }
    listen(sockfd, 128);
    return sockfd;
}

/**
 * @brief       启动 Reactor 异步事件循环 (主程序的永动机)
 * @param[in]   port       监听端口号
 * @param[in]   handler    业务逻辑处理器的函数指针 (协议解析入口)
 * @param[in]   master_fd  用于集群主从同步的文件描述符 (若非 Slave 则传 -1)
 * @return      int        服务终止返回码
 */
int reactor_start(unsigned short port, msg_handler handler, int master_fd) {
    kvs_handler = handler;
    epfd = epoll_create(1);

    int sockfd = r_init_server(port);
    if (sockfd < 0) return -1;

    init_conn_buffer(sockfd);
    set_event(sockfd, EPOLLIN, 1);

    if (master_fd > 0) {
        init_conn_buffer(master_fd);
        set_event(master_fd, EPOLLIN, 1);
        printf("[System] Slave is now listening to Master updates (FD: %d)\n", master_fd);
    }

    printf("Reactor (Epoll) Server Started on Port: %d (Dynamic Buffer: 1MB)\n", port);

    struct epoll_event events[1024];
    extern volatile int g_keep_running;

    static time_t last_check_time = 0;

    /* =========================================================
     * 核心生命周期与异步事件主循环 (The Infinite Event Loop)
     * ========================================================= */
    while (g_keep_running) {
        /* 架构决策：采用极限的 1ms 超时时间，换取 CPU 时间片去嗅探 XDP 共享内存 */
        int nready = epoll_wait(epfd, events, 1024, 1);
        
        /* 旁路嗅探：无锁拉取 eBPF/XDP BPF Map 中截获的网络报文 */
        kvs_shm_rx_poll();

        for (int i = 0; i < nready; i++) {
            int connfd = events[i].data.fd;

            if (connfd == sockfd) {
                accept_cb(connfd);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                recv_cb(connfd);
            } 
            else if (events[i].events & EPOLLOUT) {
                send_cb(connfd);
            }
        }

        /* 系统空闲时隙维护：执行兜底数据刷盘与尾包强制推送 */
        if (nready == 0) {
            kvs_aof_flush_to_kernel();
            
            /* 保证主从同步微批处理缓冲区中不足 MTU 的碎片报文不被长期滞留 */
            xdp_flush_to_slave(); 
        }

        /* 异步 I/O 回收：收割 io_uring 中已经落盘完毕的完成事件 (CQEs) */
        kvs_persist_reap_completions();

        /* 持久化审计：触发 AOF 文件的空间放大检查与后台重写机制 */
        time_t now = time(NULL);
        if (now > last_check_time) {
            kvs_aof_auto_rewrite_check();
            last_check_time = now;
        }
    }
    
    /* =========================================================
     * 优雅降级与资源回收 (Graceful Teardown)
     * ========================================================= */
    printf("\n[Teardown] Cleaning up Epoll and Network Buffers...\n");
    
    /* 强制排空最后残存的内核态与旁路网络缓冲区，确保数据强一致性 */
    kvs_aof_flush_to_kernel();
    xdp_flush_to_slave();
    
    /* 遍历并释放全局连接池中持有的所有动态内存 */
    for (int i = 0; i < CONNECTION_SIZE; i++) {
        if (conn_list[i].rbuffer != NULL || conn_list[i].wbuffer != NULL) {
            close_conn(i);
        }
    }
    
    close(epfd);
    return 0;
}