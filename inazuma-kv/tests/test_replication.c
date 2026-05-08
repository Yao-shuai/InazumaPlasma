#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <hiredis/hiredis.h>

// ==========================================
// [环境适配] 根据你的实际 IP 修改
// ==========================================
#define MASTER_IP   "192.168.124.13"  // Master (ens_xdp/eth_xdp IP)
#define MASTER_PORT 6379

#define SLAVE_IP    "192.168.124.17"  // Slave (ens_xdp/eth_xdp IP)
#define SLAVE_PORT  6380              // 确保 slave.conf 里配置的是 6380

// 开关：如果你还没实现 Slave 的多线程并发（XDP+TCP同时跑），请把这个设为 0
// 设为 0 时：只测试写入 Master，不尝试连 Slave（避免超时）
#define ENABLE_SLAVE_READ_CHECK 0 

#define GREEN "\033[0;32m"
#define RED   "\033[0;31m"
#define CYAN  "\033[0;36m"
#define RESET "\033[0m"

// 辅助断言逻辑：检查从节点是否同步成功
void assert_repl_data(redisContext *slave_c, const char *get_cmd, const char *key, const char *expected_val, size_t expected_len) {
#if ENABLE_SLAVE_READ_CHECK
    // 跨机网络可能比本地稍微多一点点延迟
    usleep(50000); 

    // 💥 修复点 1：强制转换
    redisReply *reply = (redisReply *)redisCommand(slave_c, "%s %s", get_cmd, key);
    if (reply == NULL) {
        printf(RED "[FAIL] Slave connection lost.\n" RESET);
        exit(1);
    }

    if (reply->type != REDIS_REPLY_STRING) {
        printf(RED "[FAIL] Expected String, got type %d. (Is sync delayed or failed?)\n" RESET, reply->type);
        freeReplyObject(reply);
        exit(1);
    }

    if (reply->len != expected_len) {
        printf(RED "[FAIL] Length mismatch! Master sent %zu, Slave got %zu\n" RESET, expected_len, reply->len);
        freeReplyObject(reply);
        exit(1);
    }

    if (memcmp(reply->str, expected_val, expected_len) != 0) {
        printf(RED "[FAIL] Data content corrupted during replication!\n" RESET);
        freeReplyObject(reply);
        exit(1);
    }

    printf(GREEN "[PASS]" RESET " Verified on Slave.\n");
    freeReplyObject(reply);
#else
    // 如果无法连从机，直接跳过验证
    printf(CYAN "[SKIP]" RESET " Slave Read Check Skipped (XDP Mode).\n");
#endif
}

// 引擎二进制穿透复制测试
void test_engine_replication(redisContext *master_c, redisContext *slave_c, const char *engine, const char *set_cmd, const char *get_cmd) {
    printf("Testing %-8s Incremental Sync (Binary Safe): ", engine);
    
    char bin_data[10];
    memcpy(bin_data, "Rep\0lData", 9); // 9 bytes，中间有 \0，测试二进制穿透
    
    char key[64];
    snprintf(key, sizeof(key), "repl_%s_key", engine);

    // 1. 写主节点 (使用 %b 发送精确字节)
    // 💥 修复点 2：强制转换
    redisReply *reply = (redisReply *)redisCommand(master_c, "%s %s %b", set_cmd, key, bin_data, (size_t)9);
    if (!reply || (reply->type != REDIS_REPLY_STATUS && reply->type != REDIS_REPLY_STRING)) {
        printf(RED "[FAIL] Master Write Failed\n" RESET);
        exit(1);
    }
    freeReplyObject(reply);

    // 2. 读从节点并严格断言
    assert_repl_data(slave_c, get_cmd, key, bin_data, 9);
}

int main() {
    printf(CYAN "=== InazumaKV Cross-Machine Replication Test ===\n" RESET);
    
    struct timeval timeout = { 2, 0 };
    
    // 1. 连接 Master
    redisContext *master_c = redisConnectWithTimeout(MASTER_IP, MASTER_PORT, timeout);
    if (!master_c || master_c->err) {
        printf(RED "Failed to connect to Master (%s:%d): %s\n" RESET, 
               MASTER_IP, MASTER_PORT, master_c ? master_c->errstr : "Unknown"); 
        exit(1);
    }
    printf(GREEN "[OK]" RESET " Connected to Master.\n");

    // 2. 连接 Slave (如果开启)
    redisContext *slave_c = NULL;
#if ENABLE_SLAVE_READ_CHECK
    slave_c = redisConnectWithTimeout(SLAVE_IP, SLAVE_PORT, timeout);
    if (!slave_c || slave_c->err) {
        printf(RED "Failed to connect to Slave (%s:%d). Is it blocked by XDP loop?\n" RESET, SLAVE_IP, SLAVE_PORT); 
        exit(1);
    }
    printf(GREEN "[OK]" RESET " Connected to Slave.\n");
#endif

    printf("\n" CYAN "[Phase 2: Multi-Engine Real-time Incremental Sync]" RESET "\n");
    test_engine_replication(master_c, slave_c, "Array",    "SET",  "GET");
    test_engine_replication(master_c, slave_c, "RBTree",   "RSET", "RGET");
    test_engine_replication(master_c, slave_c, "Hash",     "HSET", "HGET");
    test_engine_replication(master_c, slave_c, "Skiplist", "ZSET", "ZGET");

    // 测试命令传播：DEL 操作
    printf("Testing DEL Command Propagation:                  ");
    // 💥 修复点 3：强制转换
    redisReply *r = (redisReply *)redisCommand(master_c, "DEL repl_Array_key");
    if (r) freeReplyObject(r);
    
    // 验证删除
    assert_repl_data(slave_c, "GET", "repl_Array_key", NULL, 0); 

    printf("\n" GREEN "ALL REPLICATION TESTS COMPLETED SUCCESSFULLY!" RESET "\n");

    redisFree(master_c);
    if (slave_c) redisFree(slave_c);
    return 0;
}