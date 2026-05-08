#include "nty_coroutine.h"
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include "kvstore.h"
#include "kvs_replication.h" // 引入复制模块

// 将上下文定义得更轻量，且包含 FD 副本
struct conn {
    int fd;
    char rbuffer[KVS_MAX_MSG_LEN]; 
    int rlength;                   
    char wbuffer[KVS_MAX_MSG_LEN];
    int wlength;
};

typedef int (*msg_handler)(char *msg, int length, char *response, int *processed);
static msg_handler kvs_handler;

void server_reader(void *arg) {
    // 1. 获取 FD 并释放参数内存
    int fd = *(int *)arg;
    free(arg); 

    struct conn *c = (struct conn *)calloc(1, sizeof(struct conn));
    if (!c) {
        close(fd);
        return;
    }
    c->fd = fd;

    while (1) {
        int remain = KVS_MAX_MSG_LEN - c->rlength;
        if (remain <= 0) break;

        // nty_coroutine hook 的 recv
        int count = recv(fd, c->rbuffer + c->rlength, remain, 0);
        
        if (count > 0) {
            c->rlength += count;

            // 2. Replication 识别逻辑
            if (c->rlength >= 5 && strncmp(c->rbuffer, "PSYNC", 5) == 0) {
                kvs_replication_add_slave(fd);
       
                // printf("[Master] Slave registered on FD: %d\n", fd);
            }

            // 循环处理业务指令
            while (c->rlength > 0) {
                int processed_once = 0;
                c->wlength = kvs_handler(c->rbuffer, c->rlength, c->wbuffer, &processed_once);

                extern kvs_replication_t g_repl; // 引用全局复制状态
                if (g_repl.role == KVS_ROLE_SLAVE && fd == g_repl.master_fd) {
                    c->wlength = 0; 
                }

                if (processed_once > 0) {
                    if (c->wlength > 0) {
                        send(fd, c->wbuffer, c->wlength, 0);
                    }
                    
                    // 滑动缓冲区
                    int remain_data = c->rlength - processed_once;
                    if (remain_data > 0) {
                        memmove(c->rbuffer, c->rbuffer + processed_once, remain_data);
                    }
                    c->rlength = remain_data;
                } else {
                    break; // 数据不足，继续 recv
                }
            }
        } else if (count == 0) {
            break; // 关闭
        } else {
            // 处理 EAGAIN (虽然 ntyco 通常处理了，但加上更保险)
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break; 
        }
    }

    close(fd);
    free(c);
}

void server_accept(void *arg) {
    unsigned short port = *(unsigned short *)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(fd, (struct sockaddr*)&local, sizeof(struct sockaddr_in)) < 0) {
        close(fd);
        return;
    }

    listen(fd, 1024);
    printf("NtyCo KVS ready on port: %d\n", port);

    while (1) {
        struct sockaddr_in remote;
        socklen_t len = sizeof(struct sockaddr_in);
        int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);
        
        if (cli_fd > 0) {
            // 确保协程读取的是独立的内存地址
            int *p_fd = malloc(sizeof(int));
            *p_fd = cli_fd;

            nty_coroutine *read_co = NULL;
            nty_coroutine_create(&read_co, server_reader, p_fd);
        }
    }
}

// 当 kvs_replication.c 建立连接后，调用此函数将 FD 交给 NtyCo 调度
void event_register(int fd, int event) {
    if (fd < 0) return;
    
    int *p_fd = malloc(sizeof(int));
    *p_fd = fd;

    nty_coroutine *co = NULL;
    nty_coroutine_create(&co, server_reader, p_fd);
    
}

int ntyco_start(unsigned short port, msg_handler handler) {
    kvs_handler = handler;
    nty_coroutine *co = NULL;
    nty_coroutine_create(&co, server_accept, &port);
    nty_schedule_run();
    return 0;
}