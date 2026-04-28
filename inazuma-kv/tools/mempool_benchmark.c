#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h> 

// --- 配置区域 ---
#define SERVER_IP "127.0.0.1" 
#define SERVER_PORT 6379

// 100万次请求
#define REQUEST_COUNT 1000000 
#define VALUE_SIZE 128    

// ---------------------------------------------------------
// RESP 协议编码 (Industrial Standard)
// 格式: *3\r\n$4\r\nRSET\r\n$key_len\r\nkey\r\n$val_len\r\nval\r\n
// ---------------------------------------------------------
int encode_resp(char *buffer, const char *cmd, const char *key, const char *value) {
    int pos = 0;
    int arg_count = 2; // 至少有 CMD 和 Key
    if (value) arg_count = 3; // 如果有 Value，则是 3 参数

    // 1. 写入数组头 *N
    pos += sprintf(buffer + pos, "*%d\r\n", arg_count);

    // 2. 写入 CMD
    pos += sprintf(buffer + pos, "$%lu\r\n%s\r\n", (unsigned long)strlen(cmd), cmd);

    // 3. 写入 Key
    pos += sprintf(buffer + pos, "$%lu\r\n%s\r\n", (unsigned long)strlen(key), key);

    // 4. 写入 Value (如果有)
    if (value) {
        pos += sprintf(buffer + pos, "$%lu\r\n%s\r\n", (unsigned long)strlen(value), value);
    }
    
    return pos;
}

// ---------------------------------------------------------
// 获取高精度时间 (秒)
// ---------------------------------------------------------
double get_wall_time() {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) {
        return 0;
    }
    return (double)time.tv_sec + (double)time.tv_nsec * .000000001;
}

// ---------------------------------------------------------
// 获取服务器内存 
// ---------------------------------------------------------
void get_server_memory(long *vsz_kb, long *rss_kb) {
    FILE *fp;
    char path[1035];
    *vsz_kb = 0;
    *rss_kb = 0;

    // ps -C kvstore -o vsz,rss --no-headers
    fp = popen("ps -C kvstore -o vsz,rss --no-headers 2>/dev/null", "r");
    if (fp == NULL) {
        return;
    }

    if (fgets(path, sizeof(path), fp) != NULL) {
        sscanf(path, "%ld %ld", vsz_kb, rss_kb);
    }
    pclose(fp);
}

// ---------------------------------------------------------
// 网络发送函数
// ---------------------------------------------------------
void send_cmd_raw(int sock, const char *data, int len) {
    if (send(sock, data, len, 0) < 0) {
        perror("Send failed");
        exit(1);
    }

    char buffer[1024] = {0};
    int n = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        printf("Server closed or error.\n");
        exit(1);
    }
}

int main() {
    int sock;
    struct sockaddr_in server;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        printf("Could not create socket\n");
        return 1;
    }

    server.sin_addr.s_addr = inet_addr(SERVER_IP);
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_PORT);

    printf("Connecting to %s:%d ...\n", SERVER_IP, SERVER_PORT);
    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("Connect failed");
        return 1;
    }
    printf("Connected! Starting Benchmark (RESP Mode)...\n");

    char *large_value = (char *)malloc(VALUE_SIZE + 1);
    memset(large_value, 'A', VALUE_SIZE);
    large_value[VALUE_SIZE] = '\0';
    
    // RESP 协议会增加一些头部长，缓冲区稍微大一点
    char cmd_buffer[2048]; 
    char key_temp[64];
    
    double start_time, current_time;
    long vsz = 0, rss = 0;

    // =====================================================
    // Phase 1: 填满内存 (RSET) - 使用 RESP 协议
    // =====================================================
    printf("\n=== Phase 1: Filling Memory (RSET %d keys) ===\n", REQUEST_COUNT);
    printf("%-15s %-15s %-15s %-15s\n", "Progress", "QPS", "VSZ(MB)", "RSS(MB)");
    printf("------------------------------------------------------------\n");
    
    start_time = get_wall_time();

    for (int i = 0; i < REQUEST_COUNT; i++) {
        // 构造 Key 字符串
        sprintf(key_temp, "key_%d", i);
        
        // 编码为 RESP 协议
        int len = encode_resp(cmd_buffer, "RSET", key_temp, large_value);
        
        // 发送二进制安全的包
        send_cmd_raw(sock, cmd_buffer, len);

        if (i > 0 && i % 20000 == 0) {
            current_time = get_wall_time();
            double elapsed = current_time - start_time;
            double qps = i / elapsed;
            
            get_server_memory(&vsz, &rss);

            printf("\r%-15d %-15.2f %-15.2f %-15.2f", 
                i, qps, vsz/1024.0, rss/1024.0);
            fflush(stdout);
        }
    }
    
    current_time = get_wall_time();
    printf("\n[Phase 1 Finished] Avg QPS: %.2f\n", REQUEST_COUNT / (current_time - start_time));

    printf("\n=== Phase 1 Cooldown (Peak Memory Check) ===\n");
    printf("%-15s %-15s %-15s\n", "Time Left", "VSZ(MB)", "RSS(MB)");
    
    for(int k=0; k<5; k++) {
        sleep(1);
        get_server_memory(&vsz, &rss);
        printf("\r%-15ds %-15.2f %-15.2f", 5-k, vsz/1024.0, rss/1024.0);
        fflush(stdout);
    }
    printf("\n");

    // =====================================================
    // Phase 2: 制造碎片 (连续范围删除) - 使用 RESP 协议
    // =====================================================
    int delete_count = REQUEST_COUNT / 2;
    printf("\n=== Phase 2: Deleting Continuous Range (0 to %d) ===\n", delete_count);
    printf("%-15s %-15s %-15s %-15s\n", "Progress", "QPS", "VSZ(MB)", "RSS(MB)");
    printf("------------------------------------------------------------\n");

    start_time = get_wall_time(); 

    for (int i = 0; i < delete_count; i++) {
        // 构造 Key 字符串
        sprintf(key_temp, "key_%d", i);

        // 编码为 RESP 协议 (Value 为 NULL)
        int len = encode_resp(cmd_buffer, "RDEL", key_temp, NULL);
        
        send_cmd_raw(sock, cmd_buffer, len);

        if (i > 0 && i % 20000 == 0) {
            current_time = get_wall_time();
            double elapsed = current_time - start_time;
            double qps = i / elapsed;
            
            get_server_memory(&vsz, &rss);

            printf("\r%-15d %-15.2f %-15.2f %-15.2f", 
                i, qps, vsz/1024.0, rss/1024.0);
            fflush(stdout);
        }
    }

    current_time = get_wall_time();
    printf("\n[Phase 2 Finished] Avg QPS: %.2f\n", delete_count / (current_time - start_time));

    // =====================================================
    // Phase 3: 最终监控 
    // =====================================================
    printf("\n=== Test Finished. Monitoring Memory Decay ===\n");
    printf("Jemalloc usually needs about 10 seconds to return dirty pages to OS.\n");
    printf("------------------------------------------------------------\n");
    printf("%-15s %-15s %-15s\n", "Time Left", "VSZ(MB)", "RSS(MB)");

    for(int k=0; k<5; k++) {
        sleep(1);
        get_server_memory(&vsz, &rss);
        
        printf("\r%-15ds %-15.2f %-15.2f", 
            5-k, vsz/1024.0, rss/1024.0);
        fflush(stdout);
    }
    
    // 定格最终结果
    get_server_memory(&vsz, &rss);
    printf("\n------------------------------------------------------------\n");
    printf("FINAL RESULT:\n");
    printf("  Virtual Memory (VSZ): %.2f MB\n", vsz/1024.0);
    printf("  Physical Memory (RSS): %.2f MB\n", rss/1024.0);
    printf("------------------------------------------------------------\n");
    
    printf("\nDone. Press ENTER to exit...");
    getchar();

    close(sock);
    free(large_value);
    return 0;
}