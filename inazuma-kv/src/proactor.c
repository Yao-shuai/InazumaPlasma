#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <errno.h>

#include "kvstore.h"
#include "kvs_config.h"      
#include "kvs_replication.h" 

#define EVENT_ACCEPT    0
#define EVENT_READ      1
#define EVENT_WRITE     2

#define CONNECTION_SIZE 1024
#define ENTRIES_LENGTH  2048

// 保持 1MB 不变，保证 Pipeline 性能
#undef BUFFER_LENGTH
#define BUFFER_LENGTH   (1024 * 1024) 

// [修改 1] 将数组改为指针，大幅减小全局变量体积 (从 2GB -> 32KB)
struct conn {
    int fd;
    char *rbuffer; // 改为指针
    int rlength;
    char *wbuffer; // 改为指针
    int wlength;
};

// 全局连接数组 (现在它很小了，只有指针)
struct conn conn_list[CONNECTION_SIZE];

struct conn_info {
    int fd;
    int event;
};

struct io_uring ring;

typedef int (*msg_handler)(int client_fd, char *msg, int length, char *response, int *processed);
static msg_handler kvs_handler;

// ---------------------------------------------------------
// 辅助函数：连接初始化 (内存懒加载)
// ---------------------------------------------------------
void init_conn_buffer(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;
    
    // 如果还没分配内存，现在分配
    if (conn_list[fd].rbuffer == NULL) {
        conn_list[fd].rbuffer = (char *)malloc(BUFFER_LENGTH);
    }
    if (conn_list[fd].wbuffer == NULL) {
        conn_list[fd].wbuffer = (char *)malloc(BUFFER_LENGTH);
    }

    // 每次复用时，重置长度，但不要 free 内存（复用 Buffer 提高性能）
    conn_list[fd].rlength = 0;
    conn_list[fd].wlength = 0;
    conn_list[fd].fd = fd;
}


// ---------------------------------------------------------
// 网络基础函数
// ---------------------------------------------------------

int p_init_server(unsigned short port) {    
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);   
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serveraddr;  
    memset(&serveraddr, 0, sizeof(struct sockaddr_in)); 
    serveraddr.sin_family = AF_INET;    
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serveraddr.sin_port = htons(port);  

    if (-1 == bind(sockfd, (struct sockaddr*)&serveraddr, sizeof(struct sockaddr))) {       
        perror("bind");     
        return -1;  
    }   
    listen(sockfd, 10);
    return sockfd;
}

// ---------------------------------------------------------
// io_uring 事件提交封装
// ---------------------------------------------------------

void set_event_recv(struct io_uring *ring, int sockfd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct conn *c = &conn_list[sockfd];

    // 安全检查：防止空指针
    if (c->rbuffer == NULL) init_conn_buffer(sockfd);

    int remain = BUFFER_LENGTH - c->rlength;
    
    if (remain <= 0) {
        printf("[WARN] Buffer full for FD %d. Stop receiving.\n", sockfd);
        return; 
    }

    struct conn_info *info = malloc(sizeof(struct conn_info));
    info->fd = sockfd;
    info->event = EVENT_READ;

    io_uring_prep_recv(sqe, sockfd, c->rbuffer + c->rlength, remain, 0);
    io_uring_sqe_set_data(sqe, info);
}

void set_event_send(struct io_uring *ring, int sockfd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct conn *c = &conn_list[sockfd];

    // 安全检查
    if (c->wbuffer == NULL) init_conn_buffer(sockfd);

    struct conn_info *info = malloc(sizeof(struct conn_info));
    info->fd = sockfd;
    info->event = EVENT_WRITE;
    
    io_uring_prep_send(sqe, sockfd, c->wbuffer, c->wlength, 0);
    io_uring_sqe_set_data(sqe, info);
}

void set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr,
                      socklen_t *addrlen) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct conn_info *info = malloc(sizeof(struct conn_info));
    info->fd = sockfd;
    info->event = EVENT_ACCEPT;
    io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, 0);
    io_uring_sqe_set_data(sqe, info);
}

// [修改 2] 注册新连接 (适配动态分配)
void event_register(int fd, int event) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;

    // 动态分配/复用内存
    init_conn_buffer(fd);
    
    set_event_recv(&ring, fd);
}

// ---------------------------------------------------------
// Proactor 主循环
// ---------------------------------------------------------

int proactor_start(unsigned short port, msg_handler handler, int master_fd) {
    int sockfd = p_init_server(port);
    kvs_handler = handler;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params);

    // [修改 3] 初始化 Master 连接
    if (master_fd > 0) {
        init_conn_buffer(master_fd); // 分配内存
        set_event_recv(&ring, master_fd);
        printf("[System] Slave is now listening to Master updates (FD: %d)\n", master_fd);
    }

    struct sockaddr_in clientaddr;  
    socklen_t len = sizeof(clientaddr);
    
    set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len);
    
    printf("Proactor (io_uring) Server Started on Port: %d (Dynamic Buffer: 1MB)\n", port);

    while (1) {
        io_uring_submit(&ring);

        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);

        struct io_uring_cqe *cqes[128];
        int nready = io_uring_peek_batch_cqe(&ring, cqes, 128); 

        for (int i = 0; i < nready; i++) {
            struct io_uring_cqe *entries = cqes[i];
            struct conn_info *result = (struct conn_info *)io_uring_cqe_get_data(entries);

            if (result->event == EVENT_ACCEPT) {
                // 1. Accept
                int connfd = entries->res;
                if (connfd >= 0) {
                    // [修改 4] 新连接进来时，分配内存
                    init_conn_buffer(connfd);
                    set_event_recv(&ring, connfd);
                }
                set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len);
                
            } else if (result->event == EVENT_READ) { 
                // 2. Read
                int ret = entries->res;
                int fd = result->fd;
                struct conn *c = &conn_list[fd];

                if (ret <= 0) {
                    close(fd);
                    if (fd == g_repl.master_fd) printf("[WARN] Master connection lost!\n");
                    // 注意：这里我们选择不 free 内存，而是留给下一个复用该 FD 的连接
                    // 这样可以避免频繁的 malloc/free 开销 (Slab 思想)
                } else {
                    c->rlength += ret;

                    int processed = 0;
                    c->wlength = kvs_handler(fd, c->rbuffer, c->rlength, c->wbuffer, &processed);
                    
                    if (g_repl.role == ROLE_SLAVE && fd == g_repl.master_fd) {
                        c->wlength = 0; 
                    }

                    // Buffer 搬运
                    if (processed > 0) {
                        if (c->rlength > processed) {
                            int remain = c->rlength - processed;
                            memmove(c->rbuffer, c->rbuffer + processed, remain);
                            c->rlength = remain;
                        } else {
                            c->rlength = 0;
                        }
                    }

                    if (c->wlength > 0) {
                        set_event_send(&ring, fd);
                    } else {
                        set_event_recv(&ring, fd);
                    }
                }

            }  else if (result->event == EVENT_WRITE) {
                // 3. Write
                int fd = result->fd;
                struct conn *c = &conn_list[fd];
                c->wlength = 0; 
                set_event_recv(&ring, fd);
            }
            
            free(result);
        }
        io_uring_cq_advance(&ring, nready);
    }
}