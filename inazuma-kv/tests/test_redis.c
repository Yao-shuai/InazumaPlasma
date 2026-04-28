#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <hiredis/hiredis.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 6379

// 终端颜色输出宏
#define GREEN "\033[0;32m"
#define RED   "\033[0;31m"
#define CYAN  "\033[0;36m"
#define RESET "\033[0m"

long long current_timestamp_ms() {
    struct timeval te; 
    gettimeofday(&te, NULL);
    return te.tv_sec*1000LL + te.tv_usec/1000;
}

// =========================================================================
// 断言辅助函数库
// =========================================================================
void check_error(redisContext *c, redisReply *reply) {
    if (reply == NULL) {
        printf(RED "[FAIL] Connection lost or Protocol Error: %s\n" RESET, c->errstr);
        exit(1);
    }
}

// 断言字符串或状态 (OK / String)
void assert_string(redisContext *c, redisReply *reply, const char *expected_val) {
    check_error(c, reply);
    if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS) {
        if (memcmp(reply->str, expected_val, strlen(expected_val)) != 0) {
            printf(RED "[FAIL] Expected '%s', Got '%s'\n" RESET, expected_val, reply->str);
            exit(1);
        }
    } else {
        printf(RED "[FAIL] Type Mismatch. Expected String/Status, Got type %d\n" RESET, reply->type);
        exit(1);
    }
    printf(GREEN "[PASS] " RESET);
}

// 断言整数 (用于 EXIST, DEL 返回的 :1 / :0)
void assert_integer(redisContext *c, redisReply *reply, long long expected_val) {
    check_error(c, reply);
    if (reply->type != REDIS_REPLY_INTEGER) {
        printf(RED "[FAIL] Expected Integer, Got type %d\n" RESET, reply->type);
        exit(1);
    }
    if (reply->integer != expected_val) {
        printf(RED "[FAIL] Expected %lld, Got %lld\n" RESET, expected_val, reply->integer);
        exit(1);
    }
    printf(GREEN "[PASS] " RESET);
}

// 断言空值 (用于 GET 不存在的 Key 返回的 $-1\r\n)
void assert_nil(redisContext *c, redisReply *reply) {
    check_error(c, reply);
    if (reply->type != REDIS_REPLY_NIL) {
        printf(RED "[FAIL] Expected NIL, Got type %d\n" RESET, reply->type);
        exit(1);
    }
    printf(GREEN "[PASS] " RESET);
}

// =========================================================================
// 阶段一：全引擎 CRUD 与边界测试
// =========================================================================
void test_engine_crud(redisContext *c, const char *engine, const char *cmd_set, const char *cmd_get, const char *cmd_mod, const char *cmd_del, const char *cmd_exist) {
    printf(CYAN "\n--- Testing %s Engine (CRUD & Boundary) ---\n" RESET, engine);
    redisReply *reply;
    char key[64];
    snprintf(key, sizeof(key), "%s_key", engine);

    // 1. 基础 SET
    printf("1. Basic SET:      ");
    // 💥 修复点：强制转换为 (redisReply *)
    reply = (redisReply *)redisCommand(c, "%s %s %s", cmd_set, key, "value_1");
    assert_string(c, reply, "OK");
    freeReplyObject(reply);
    printf("\n");

    // 2. 基础 GET
    printf("2. Basic GET:      ");
    reply = (redisReply *)redisCommand(c, "%s %s", cmd_get, key);
    assert_string(c, reply, "value_1");
    freeReplyObject(reply);
    printf("\n");

    // 3. EXIST 校验
    printf("3. Check EXIST:    ");
    reply = (redisReply *)redisCommand(c, "%s %s", cmd_exist, key);
    assert_integer(c, reply, 1); 
    freeReplyObject(reply);
    printf("\n");

    // 4. 更新值 MOD
    printf("4. Update MOD:     ");
    reply = (redisReply *)redisCommand(c, "%s %s %s", cmd_mod, key, "value_2_updated");
    assert_string(c, reply, "OK");
    freeReplyObject(reply);
    printf("\n");

    // 5. 特殊字符测试 (空格, JSON)
    printf("5. Space & JSON:   ");
    // 💥 修复点：加上 const 修饰符
    const char *json_val = "{\"msg\": \"hello world\"}";
    reply = (redisReply *)redisCommand(c, "%s %s_json %s", cmd_set, key, json_val);
    assert_string(c, reply, "OK");
    freeReplyObject(reply);
    
    reply = (redisReply *)redisCommand(c, "%s %s_json", cmd_get, key);
    assert_string(c, reply, json_val);
    freeReplyObject(reply);
    printf("\n");

    // 6. 删除 DEL
    printf("6. Delete DEL:     ");
    reply = (redisReply *)redisCommand(c, "%s %s", cmd_del, key);
    assert_integer(c, reply, 1); 
    freeReplyObject(reply);
    printf("\n");

    // 7. 删除后的状态校验
    printf("7. Check DELETED:  ");
    reply = (redisReply *)redisCommand(c, "%s %s", cmd_exist, key);
    assert_integer(c, reply, 0); 
    freeReplyObject(reply);
    
    reply = (redisReply *)redisCommand(c, "%s %s", cmd_get, key);
    assert_nil(c, reply); 
    freeReplyObject(reply);
    printf("\n");
}

// =========================================================================
// 阶段二：全引擎二进制安全测试 (\0)
// =========================================================================
void test_engine_binary_safety(redisContext *c, const char *engine_name, const char *set_cmd, const char *get_cmd) {
    printf("Testing %-8s Binary Data (\\0): ", engine_name);
    
    char bin_data[10];
    memcpy(bin_data, "Head\0Tail", 9); 
    
    char key[64];
    snprintf(key, sizeof(key), "%s_bin_key", engine_name);

    // 1. 写入数据 (使用 %b)
    // 💥 修复点：强制转换为 (redisReply *)
    redisReply *reply = (redisReply *)redisCommand(c, "%s %s %b", set_cmd, key, bin_data, (size_t)9);
    check_error(c, reply);
    freeReplyObject(reply);

    // 2. 读取并严格校验
    reply = (redisReply *)redisCommand(c, "%s %s", get_cmd, key);
    check_error(c, reply);

    if (reply->type != REDIS_REPLY_STRING) {
         printf(RED "[FAIL] Not a string\n" RESET);
    } else if (reply->len != 9) {
         printf(RED "[FAIL] Length mismatch. Expected 9, Got %zu (Truncated!)\n" RESET, reply->len);
         exit(1);
    } else if (memcmp(reply->str, bin_data, 9) != 0) {
         printf(RED "[FAIL] Content mismatch.\n" RESET);
         exit(1);
    } else {
         printf(GREEN "[PASS]" RESET " Verified 9 bytes.\n");
    }
    freeReplyObject(reply);
}

// =========================================================================
// 阶段三：全引擎性能对比测试
// =========================================================================
void test_engine_performance(redisContext *c, const char *engine, const char *cmd_set, int count) {
    long long start = current_timestamp_ms();
    
    for (int i = 0; i < count; i++) {
        // 💥 修复点：强制转换为 (redisReply *)
        redisReply *r = (redisReply *)redisCommand(c, "%s %s_perf_%d %d", cmd_set, engine, i, i);
        if (r) freeReplyObject(r);
        else { printf(RED "Error at %d\n" RESET, i); break; }
    }
    
    long long end = current_timestamp_ms();
    long long duration = end - start;
    double qps = (double)count / (duration / 1000.0);

    printf("%-10s -> Time: %4lld ms | QPS : " GREEN "%.2f ops/sec" RESET "\n", engine, duration, qps);
}

// =========================================================================
// 主程序
// =========================================================================
int main(int argc, char **argv) {
    printf("Connecting to %s:%d...\n", SERVER_IP, SERVER_PORT);
    
    struct timeval timeout = { 2, 0 }; 
    redisContext *c = redisConnectWithTimeout(SERVER_IP, SERVER_PORT, timeout);
    
    if (c == NULL || c->err) {
        printf(RED "Connection error: %s\n" RESET, c ? c->errstr : "can't allocate redis context");
        exit(1);
    }
    printf("Connected successfully.\n");

    printf("\n" CYAN "========================================================" RESET);
    printf("\n" CYAN "        PHASE 1: COMPREHENSIVE CRUD & BOUNDARY          " RESET);
    printf("\n" CYAN "========================================================" RESET "\n");
    test_engine_crud(c, "Array",    "SET",  "GET",  "MOD",  "DEL",  "EXIST");
    test_engine_crud(c, "RBTree",   "RSET", "RGET", "RMOD", "RDEL", "REXIST");
    test_engine_crud(c, "Hash",     "HSET", "HGET", "HMOD", "HDEL", "HEXIST");
    test_engine_crud(c, "Skiplist", "ZSET", "ZGET", "ZMOD", "ZDEL", "ZEXIST");

    printf("\n" CYAN "========================================================" RESET);
    printf("\n" CYAN "        PHASE 2: BINARY SAFETY ABUSE TEST (\\0)          " RESET);
    printf("\n" CYAN "========================================================" RESET "\n");
    test_engine_binary_safety(c, "Array",    "SET",  "GET");
    test_engine_binary_safety(c, "RBTree",   "RSET", "RGET");
    test_engine_binary_safety(c, "Hash",     "HSET", "HGET");
    test_engine_binary_safety(c, "Skiplist", "ZSET", "ZGET");

    int perf_count = 10000;
    printf("\n" CYAN "========================================================" RESET);
    printf("\n" CYAN "        PHASE 3: PERFORMANCE BENCHMARK (%d Ops)       " RESET, perf_count);
    printf("\n" CYAN "========================================================" RESET "\n");
    printf("Note: This tests single-thread round-trip latency. \nUse pipeline_test for max throughput.\n\n");
    
    test_engine_performance(c, "Array",    "SET",  perf_count);
    test_engine_performance(c, "RBTree",   "RSET", perf_count);
    test_engine_performance(c, "Hash",     "HSET", perf_count);
    test_engine_performance(c, "Skiplist", "ZSET", perf_count);

    printf("\n" GREEN "ALL TESTS COMPLETED SUCCESSFULLY!" RESET "\n");

    redisFree(c);
    return 0;
}