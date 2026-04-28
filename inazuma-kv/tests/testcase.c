#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_MSG_LENGTH      1024
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)
#define TEST_ITERATIONS     10000 // 统一压测次数 (1万次循环 * 9个指令 = 9万 Ops)

// 💥 修复点：加上 const 修饰符
int send_msg(int connfd, const char *msg, int length) {
    int res = send(connfd, msg, length, 0);
    if (res < 0) {
        perror("send");
        exit(1);
    }
    return res;
}

int recv_msg(int connfd, char *msg, int length) {
    int res = recv(connfd, msg, length, 0);
    if (res < 0) {
        perror("recv");
        exit(1);
    }
    return res;
}

// 💥 修复点：为所有传入静态字符串常量的参数加上 const 修饰符
void testcase(int connfd, const char *msg, const char *pattern, const char *casename) {
    if (!msg || !pattern || !casename) return;

    char cmd_with_crlf[MAX_MSG_LENGTH] = {0};
    snprintf(cmd_with_crlf, sizeof(cmd_with_crlf), "%s\r\n", msg);
    send_msg(connfd, cmd_with_crlf, strlen(cmd_with_crlf));
    
    char result[MAX_MSG_LENGTH] = {0};
    int n = recv_msg(connfd, result, MAX_MSG_LENGTH - 1);
    
    if (n > 0) {
        result[n] = '\0'; // 确保字符串正确闭合，方便 strcmp
    }

    // RESP 协议兼容与容错
    if (strcmp(result, pattern) == 0) {
        // 压测模式下静默，防刷屏
    } else if (strcmp(pattern, "+OK\r\n") == 0 && strcmp(result, "+EXIST\r\n") == 0) {
        // 兼容重入拦截
    } else {
        printf("==> FAILED -> %s, EXPECTED: '%s' BUT GOT: '%s'\n", casename, pattern, result);
        exit(1);
    }
}

int connect_tcpserver(const char *ip, unsigned short port) {
    int connfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);

    if (0 !=  connect(connfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in))) {
        perror("connect");
        return -1;
    }
    return connfd;
}

// =================================================================
// 🚀 多态引擎原子操作定义 (CRUD 闭环，确保测试后不留脏数据)
// =================================================================

void array_testcase(int connfd) {
    testcase(connfd, "SET Teacher King", "+OK\r\n", "SET");
    testcase(connfd, "GET Teacher", "$4\r\nKing\r\n", "GET");
    testcase(connfd, "MOD Teacher Darren", "+OK\r\n", "MOD");
    testcase(connfd, "GET Teacher", "$6\r\nDarren\r\n", "GET-MOD");
    testcase(connfd, "EXIST Teacher", ":1\r\n", "EXISTS");     
    testcase(connfd, "DEL Teacher", ":1\r\n", "DEL");
    testcase(connfd, "GET Teacher", "$-1\r\n", "GET-NULL");
    testcase(connfd, "MOD Teacher KING", "-ERR Key not found\r\n", "MOD-FAIL");
    testcase(connfd, "EXIST Teacher", ":0\r\n", "EXISTS-NULL"); 
}

void hash_testcase(int connfd) {
    testcase(connfd, "HSET Teacher King", "+OK\r\n", "HSET");
    testcase(connfd, "HGET Teacher", "$4\r\nKing\r\n", "HGET");
    testcase(connfd, "HMOD Teacher Darren", "+OK\r\n", "HMOD");
    testcase(connfd, "HGET Teacher", "$6\r\nDarren\r\n", "HGET-MOD");
    testcase(connfd, "HEXIST Teacher", ":1\r\n", "HEXISTS");    
    testcase(connfd, "HDEL Teacher", ":1\r\n", "HDEL");
    testcase(connfd, "HGET Teacher", "$-1\r\n", "HGET-NULL");
    testcase(connfd, "HMOD Teacher KING", "-ERR Key not found\r\n", "HMOD-FAIL");
    testcase(connfd, "HEXIST Teacher", ":0\r\n", "HEXISTS-NULL");
}

void rbtree_testcase(int connfd) {
    testcase(connfd, "RSET Teacher King", "+OK\r\n", "RSET");
    testcase(connfd, "RGET Teacher", "$4\r\nKing\r\n", "RGET");
    testcase(connfd, "RMOD Teacher Darren", "+OK\r\n", "RMOD");
    testcase(connfd, "RGET Teacher", "$6\r\nDarren\r\n", "RGET-MOD");
    testcase(connfd, "REXIST Teacher", ":1\r\n", "REXISTS");    
    testcase(connfd, "RDEL Teacher", ":1\r\n", "RDEL");
    testcase(connfd, "RGET Teacher", "$-1\r\n", "RGET-NULL");
    testcase(connfd, "RMOD Teacher KING", "-ERR Key not found\r\n", "RMOD-FAIL");
    testcase(connfd, "REXIST Teacher", ":0\r\n", "REXISTS-NULL");
}

void skiplist_testcase(int connfd) {
    testcase(connfd, "ZSET Teacher King", "+OK\r\n", "ZSET");
    testcase(connfd, "ZGET Teacher", "$4\r\nKing\r\n", "ZGET");
    testcase(connfd, "ZMOD Teacher Darren", "+OK\r\n", "ZMOD");
    testcase(connfd, "ZGET Teacher", "$6\r\nDarren\r\n", "ZGET-MOD");
    testcase(connfd, "ZEXIST Teacher", ":1\r\n", "ZEXISTS");    
    testcase(connfd, "ZDEL Teacher", ":1\r\n", "ZDEL");
    testcase(connfd, "ZGET Teacher", "$-1\r\n", "ZGET-NULL");
    testcase(connfd, "ZMOD Teacher KING", "-ERR Key not found\r\n", "ZMOD-FAIL");
    testcase(connfd, "ZEXIST Teacher", ":0\r\n", "ZEXISTS-NULL");
}

// =================================================================
// 🚀 标准化 Benchmark 压测封装
// =================================================================

void run_benchmark(int connfd, const char* engine_name, void (*test_func)(int)) {
    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        test_func(connfd);
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms
    long total_ops = TEST_ITERATIONS * 9; // 每次 loop 执行 9 个命令
    
    printf("[%s] Benchmark --> Time: %d ms | QPS: %ld ops/sec\n", 
            engine_name, time_used, (time_used > 0) ? (total_ops * 1000 / time_used) : 0);
}

// =================================================================
// 主入口
// =================================================================

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("=========================================\n");
        printf("🚀 InazumaKV Engine Benchmark Tool\n");
        printf("=========================================\n");
        printf("Usage: %s <ip> <port> <engine_mode>\n\n", argv[0]);
        printf("Engine Modes:\n");
        printf("  0 : Array    (O(N) Linear)\n");
        printf("  1 : Hash     (O(1) Constant)\n");
        printf("  2 : RBTree   (O(log N) Balanced)\n");
        printf("  3 : Skiplist (O(log N) Probabilistic)\n");
        printf("=========================================\n");
        return -1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);

    int connfd = connect_tcpserver(ip, port);
    if (connfd < 0) return -1;

    switch (mode) {
        case 0: run_benchmark(connfd, "Array", array_testcase); break;
        case 1: run_benchmark(connfd, "Hash", hash_testcase); break;
        case 2: run_benchmark(connfd, "RBTree", rbtree_testcase); break;
        case 3: run_benchmark(connfd, "Skiplist", skiplist_testcase); break;
        default: printf("❌ Invalid mode selected.\n"); break;
    }

    close(connfd);
    return 0;
}