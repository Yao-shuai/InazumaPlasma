#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define SERVER_IP "192.168.124.13" // Master IP
#define SERVER_PORT 6379

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("Socket"); return 1; }

    struct sockaddr_in server;
    server.sin_addr.s_addr = inet_addr(SERVER_IP);
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_PORT);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("Connect"); return 1;
    }

    printf("Connected to Master. Sending Latency Probes...\n");

    char buf[256];
    char resp[1024];

    while(1) {
        // 1. 获取当前时间 (微秒)
        struct timeval te;
        gettimeofday(&te, NULL);
        long long now = te.tv_sec * 1000000LL + te.tv_usec;

        // 2. 构造 RESP 命令: SET LATENCY_PROBE <timestamp>
        // 保证时间戳是 16 位，不足补 0
        int len = sprintf(buf, "*3\r\n$3\r\nSET\r\n$13\r\nLATENCY_PROBE\r\n$16\r\n%016lld\r\n", now);

        // 3. 发送
        send(sock, buf, len, 0);

        // 4. 接收响应 (必须收，否则缓冲区会满)
        recv(sock, resp, sizeof(resp), 0);

        printf("[Master] Sent probe: %lld\n", now);
        
        // 每秒发一次，方便观察
        sleep(1);
    }

    close(sock);
    return 0;
}