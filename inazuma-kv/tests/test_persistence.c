#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 6379
#define BUFFER_SIZE 4096

double get_timestamp() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int connect_server() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) return -1;
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) return -1;
    return sock;
}

int send_cmd(int sock, const char *cmd, char *response) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "%.*s\r\n", (int)(BUFFER_SIZE - 3), cmd);
    if (send(sock, buffer, strlen(buffer), 0) < 0) return -1;
    memset(response, 0, BUFFER_SIZE);
    int len = recv(sock, response, BUFFER_SIZE - 1, 0);
    return (len <= 0) ? -1 : len;
}

void benchmark_write(int count, const char *cmd_prefix) {
    int sock = connect_server();
    if (sock < 0) { perror("Connect failed"); return; }

    printf("==================================================\n");
    printf("[Test] Mode: WRITE | Engine: %s | Count: %d\n", cmd_prefix, count);
    
    char cmd[BUFFER_SIZE];
    char resp[BUFFER_SIZE];
    
    double start_time = get_timestamp();
    int success_cnt = 0;
    
    for (int i = 0; i < count; i++) {
        snprintf(cmd, BUFFER_SIZE, "%s key_%d value_%d", cmd_prefix, i, i);
        if (send_cmd(sock, cmd, resp) > 0) {
            if (strstr(resp, "OK") || strstr(resp, "EXIST")) success_cnt++;
        }
    }
    
    double end_time = get_timestamp();
    double duration = end_time - start_time;
    printf("[Write] Done. Success: %d/%d\n", success_cnt, count);
    printf("[Stats] TPS: %.2f ops/sec\n", count / duration);

    printf("[Save ] Triggering SAVE...\n");
    start_time = get_timestamp();
    send_cmd(sock, "SAVE", resp);
    end_time = get_timestamp();
    printf("[Stats] SAVE blocking time: %.4f s\n", end_time - start_time);
    
    close(sock);
}

void benchmark_verify(int count, const char *get_cmd) {
    int sock = connect_server();
    if (sock < 0) return;

    printf("[Test] Mode: VERIFY | Command: %s\n", get_cmd);
    char cmd[BUFFER_SIZE];
    char resp[BUFFER_SIZE];
    int found_cnt = 0;

    for (int i = 0; i < count; i++) {
        snprintf(cmd, BUFFER_SIZE, "%s key_%d", get_cmd, i);
        if (send_cmd(sock, cmd, resp) > 0) {
            if (strstr(resp, "NO EXIST") == NULL && strstr(resp, "ERROR") == NULL) found_cnt++;
        }
    }
    printf("[Result] Recovered: %d/%d\n", found_cnt, count);
    close(sock);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <mode> <count> <cmd_type>\n", argv[0]);
        printf("  [Mode 1 - Write ] cmd_type: SET, RSET, HSET, ZSET\n");
        printf("  [Mode 2 - Verify] cmd_type: GET, RGET, HGET, ZGET\n");
        printf("Example Write : %s 1 50000 RSET\n", argv[0]);
        printf("Example Verify: %s 2 50000 RGET\n", argv[0]);
        return 0;
    }
    int mode = atoi(argv[1]);
    int count = atoi(argv[2]);
    char *cmd_type = argv[3];

    // 极简主义：用户传什么指令，我们就用什么指令测试
    if (mode == 1) {
        benchmark_write(count, cmd_type);
    } else if (mode == 2) {
        benchmark_verify(count, cmd_type);
    } else {
        printf("Unknown mode: %d\n", mode);
    }
    
    return 0;
}