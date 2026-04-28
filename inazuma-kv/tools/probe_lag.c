#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>

#define MAX_LADS 1000

// 获取当前微秒时间戳
long long get_time_usec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

// 创建阻塞 Socket 并连接
int connect_redis(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    
    // 设置接收超时时间，防止死锁
    struct timeval tv = {0, 50000}; // 50ms 超时
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Connection failed");
        exit(1);
    }
    return fd;
}

// 用于 qsort 排序计算 P99
int compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double *)a;
    double arg2 = *(const double *)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

// ... 前面的 includes 和函数保持不变 ...

int main(int argc, char *argv[]) {
    // 🚀 升级为动态传入主从 IP 和端口
    if (argc < 5) {
        printf("Usage: %s <master_ip> <master_port> <slave_ip> <slave_port> [hash]\n", argv[0]);
        return 1;
    }

    char *m_ip = argv[1];
    int m_port = atoi(argv[2]);
    char *s_ip = argv[3];
    int s_port = atoi(argv[4]);
    int is_hash = (argc > 5 && strcmp(argv[5], "hash") == 0);

    // 动态连接
    int m_fd = connect_redis(m_ip, m_port);
    int s_fd = connect_redis(s_ip, s_port);

    printf("🔗 [Probe] 已连接 Master(%s:%d) 与 Slave(%s:%d)，开始微秒级探针发射...\n", 
           m_ip, m_port, s_ip, s_port);

    // ... 下面的测速逻辑完全不变 ...
    double lags[MAX_LADS];
    char send_buf[256];
    char recv_buf[1024];
    char probe_val[64];

    for (int i = 0; i < MAX_LADS; i++) {
        long long now = get_time_usec();
        sprintf(probe_val, "%lld", now);
        
        // 构造 RESP 协议写入 Master
        int len;
        if (is_hash) {
            len = sprintf(send_buf, "*4\r\n$4\r\nHSET\r\n$10\r\nsync_probe\r\n$3\r\nval\r\n$%zu\r\n%s\r\n", strlen(probe_val), probe_val);
        } else {
            len = sprintf(send_buf, "*3\r\n$3\r\nSET\r\n$10\r\nsync_probe\r\n$%zu\r\n%s\r\n", strlen(probe_val), probe_val);
        }

        long long t_start = get_time_usec();
        send(m_fd, send_buf, len, 0);

        // 构造 RESP 协议轮询 Slave
        if (is_hash) {
            len = sprintf(send_buf, "*3\r\n$4\r\nHGET\r\n$10\r\nsync_probe\r\n$3\r\nval\r\n");
        } else {
            len = sprintf(send_buf, "*2\r\n$3\r\nGET\r\n$10\r\nsync_probe\r\n");
        }

        while (1) {
            send(s_fd, send_buf, len, 0);
            memset(recv_buf, 0, sizeof(recv_buf));
            int r = recv(s_fd, recv_buf, sizeof(recv_buf) - 1, 0);
            
            if (r > 0 && strstr(recv_buf, probe_val) != NULL) {
                long long t_end = get_time_usec();
                lags[i] = (t_end - t_start) / 1000.0; 
                break;
            }
        }
        usleep(10000); 
    }

    qsort(lags, MAX_LADS, sizeof(double), compare_doubles);

    double sum = 0;
    for (int i = 0; i < MAX_LADS; i++) sum += lags[i];

    printf("\n📊 物理同步延迟 (Replication Lag) 报告 (C 语言极速探针)：\n");
    printf("AVG (平均): %.3f ms\n", sum / MAX_LADS);
    printf("P99 (尾部): %.3f ms\n", lags[989]);
    printf("MAX (极值): %.3f ms\n", lags[MAX_LADS - 1]);
    printf("----------------------------------------\n");

    close(m_fd);
    close(s_fd);
    return 0;
}