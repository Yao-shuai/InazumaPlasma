#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

// =================配置区域=================
#define SERVER_IP "192.168.124.13"  // 主机 IP
#define SERVER_PORT 6379              // 主机端口 (注意 master.conf 配置)
#define BATCH_SIZE 5000             // 批量大小 (加大压力测试粘包)
#define MAX_CMD_LEN 128              // 单条命令最大长度
// =========================================

// 获取当前时间戳 (毫秒)
long long current_timestamp() {
    struct timeval te;
    gettimeofday(&te, NULL);
    return te.tv_sec * 1000LL + te.tv_usec / 1000;
}

// 核心测试函数
// cmd_prefix: 命令前缀，如 "SET", "RSET", "HSET", "ZSET"
// engine_name: 用于打印日志
int perform_batch_test(int sock, const char *engine_name, const char *cmd_prefix) {
    printf("\n========================================\n");
    printf("[Test] Engine: %s | Command: %s | Batch: %d\n", engine_name, cmd_prefix, BATCH_SIZE);

    // 1. 准备发送缓冲区
    size_t send_buf_size = BATCH_SIZE * MAX_CMD_LEN;
    char *send_buf = (char *)malloc(send_buf_size);
    if (!send_buf) return -1;
    memset(send_buf, 0, send_buf_size);

    // 2. 构造批量指令 (Pipeline)
    // 格式: CMD key_i value_i
    int offset = 0;
    for (int i = 0; i < BATCH_SIZE; i++) {
        // key 和 value 带上引擎前缀，防止互相覆盖
        int len = sprintf(send_buf + offset, "%s %s_k_%d %s_v_%d\r\n", 
                          cmd_prefix, engine_name, i, engine_name, i);
        offset += len;
    }

    printf("[Client] Sending %d bytes (%.2f KB)...\n", offset, offset / 1024.0);
    
    long long start_time = current_timestamp();

    // 3. 发送数据 (一次性发送，制造粘包)
    ssize_t sent_len = send(sock, send_buf, offset, 0);
    if (sent_len < 0) {
        perror("Send failed");
        free(send_buf);
        return -1;
    }

    // 4. 接收响应
    // 预估响应大小：每个响应 "+OK\r\n" (5字节) 或 "-ERR...\r\n"
    size_t recv_buf_size = BATCH_SIZE * 64; 
    char *recv_buf = (char *)malloc(recv_buf_size);
    if (!recv_buf) {
        free(send_buf);
        return -1;
    }
    memset(recv_buf, 0, recv_buf_size);

    int total_received = 0;
    int response_count = 0; // 统计收到了多少个完整的响应 (以 \n 结尾)
    
    // 循环接收，直到收到 BATCH_SIZE 个响应
    while (response_count < BATCH_SIZE) {
        int n = recv(sock, recv_buf + total_received, recv_buf_size - total_received - 1, 0);
        if (n < 0) {
            perror("Recv error");
            break;
        }
        if (n == 0) {
            printf("[Error] Server closed connection unexpectedly.\n");
            break;
        }

        // 统计新接收数据中的换行符数量
        for (int k = 0; k < n; k++) {
            if (recv_buf[total_received + k] == '\n') {
                response_count++;
            }
        }
        
        total_received += n;
    }

    long long end_time = current_timestamp();
    float duration = (end_time - start_time) / 1000.0;

    // 5. 验证结果 (统计 OK 和 EXIST)
    int success_count = 0;
    
    // [修改点 1] 统计 OK 数量 (插入成功)
    char *p = recv_buf;
    while ((p = strstr(p, "OK")) != NULL) {
        success_count++;
        p += 2; 
    }

    // [修改点 2] 统计 EXIST 数量 (更新成功)
    p = recv_buf;
    while ((p = strstr(p, "EXIST")) != NULL) {
        success_count++;
        p += 5; 
    }

    printf("[Result] Time: %.3fs | QPS: %.2f\n", duration, BATCH_SIZE / duration);
    printf("[Verify] Sent: %d | Recv Lines: %d | 'Success' Count (OK+EXIST): %d\n", BATCH_SIZE, response_count, success_count);

    int success = 0;
    // 只要成功数等于发送数，即为通过
    if (success_count == BATCH_SIZE) {
        printf("✅ PASS\n");
        success = 1;
    } else {
        printf("❌ FAIL (Packet Loss or Logic Error)\n");
        // 如果失败，打印少量 buffer 供调试
        if (strstr(recv_buf, "ERR")) {
            printf("[Debug] Found Error in response.\n");
        }
    }

    free(send_buf);
    free(recv_buf);
    return success;
}

int main() {
    int sock;
    struct sockaddr_in server;

    // 1. 创建 Socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Could not create socket");
        return 1;
    }

    // 设置连接超时
    struct timeval timeout;      
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    server.sin_addr.s_addr = inet_addr(SERVER_IP);
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_PORT);

    printf("Connecting to %s:%d...\n", SERVER_IP, SERVER_PORT);
    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("Connect failed");
        return 1;
    }
    printf("Connected.\n");

    // ==========================================
    // 开始依次测试所有数据结构
    // ==========================================

    // 1. 测试 Array (SET)
    perform_batch_test(sock, "array", "SET");
    
    // 稍作停顿，防止缓冲区瞬间堆积过多导致客户端处理不过来（可选）
    usleep(100000); 

    // 2. 测试 RBTree (RSET)
    perform_batch_test(sock, "rbtree", "RSET");

    usleep(100000);

    // 3. 测试 Hash (HSET)
    perform_batch_test(sock, "hash", "HSET");

    usleep(100000);

    // 4. 测试 Skiplist (ZSET)
    perform_batch_test(sock, "skiplist", "ZSET");

    // ==========================================
    // 结束
    // ==========================================
    
    close(sock);
    printf("\nAll tests completed.\n");
    return 0;
}

// int main() {
//     int sock;
//     struct sockaddr_in server;

//     // 1. 创建 Socket
//     sock = socket(AF_INET, SOCK_STREAM, 0);
//     if (sock == -1) {
//         perror("Could not create socket");
//         return 1;
//     }

//     // 设置连接超时
//     struct timeval timeout;      
//     timeout.tv_sec = 5;
//     timeout.tv_usec = 0;
//     setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

//     server.sin_addr.s_addr = inet_addr(SERVER_IP);
//     server.sin_family = AF_INET;
//     server.sin_port = htons(SERVER_PORT);

//     printf("Connecting to %s:%d...\n", SERVER_IP, SERVER_PORT);
//     if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
//         perror("Connect failed");
//         return 1;
//     }
//     printf("Connected.\n");

//     // ==========================================
//     // 开始依次测试所有数据结构
//     // ==========================================
//     perform_batch_test(sock, "hash", "HSET");
    
//     // 正式测试：连续跑 5 轮，取平均值
//     printf("\n=== [Throughput Benchmark] Running 5 Rounds ===\n");
//     for(int i=0; i<5; i++) {
//         usleep(500000); // 休息 0.5s
//         perform_batch_test(sock, "hash", "HSET");
//     }
//     close(sock);

//     return 0;
// }
