/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 * 
 * Project:      InazumaAIGateway
 * File:         ai_gateway.cpp
 * Description:  AI 网关核心服务程序。
 * 基于 io_uring 处理底层网络 I/O，使用 curl_multi 调度大模型
 * 异步 API 请求，并集成 Redis 向量检索实现请求的语义缓存。
 * ==============================================================================
 */

#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>
#include <curl/curl.h>
#include <hiredis/hiredis.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cmath>
#include <csignal>
#include <atomic>
#include <functional>
#include <memory>
#include "json.hpp"

using json = nlohmann::json;

// 基础网络与缓存常量配置
#define PORT 8085               // 网关监听端口
#define TCP_BUF_SIZE 65536      // TCP 接收缓冲区大小
#define RING_ENTRIES 256        // io_uring 队列深度
#define VECTOR_DIM 1536         // 语义向量维度

// =========================================================================
// 全局指标统计
// 说明：使用原子操作以保证在无锁状态下的并发自增安全，用于监控服务运行状态。
// =========================================================================
std::atomic<int> metric_hits(0);       // 缓存命中次数
std::atomic<int> metric_misses(0);     // 缓存未命中（回源）次数
std::atomic<int> metric_qwen(0);       // qwen 模型请求次数
std::atomic<int> metric_gpt4o(0);      // gpt-4o 模型请求次数
std::atomic<int> metric_deepseek(0);   // deepseek 模型请求次数

// =========================================================================
// 服务配置模块
// =========================================================================
struct GatewayConfig { 
    std::string qwen_key; 
    std::string openai_key; 
    std::string deepseek_key; 
    std::string default_model; 
    std::string redis_host; 
    int redis_port;
};
GatewayConfig global_config;

// 移除字符串首尾的空白字符和换行符
void trim_key(std::string &s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && (s.front() == '\r' || s.front() == '\n' || s.front() == ' ')) s.erase(s.begin());
}

// 获取环境变量，若未设置则返回默认值
std::string get_env_var(const char* key, const std::string& default_val = "") {
    const char* val = std::getenv(key);
    return val ? std::string(val) : default_val;
}

// 加载网关配置，优先级：环境变量 > 配置文件 > 默认值
void load_config(const std::string& config_path) {
    global_config.default_model = "qwen-turbo";
    global_config.redis_port = 6379;
    std::string env_qwen = get_env_var("QWEN_API_KEY");
    std::string env_openai = get_env_var("OPENAI_API_KEY");
    std::string env_deepseek = get_env_var("DEEPSEEK_API_KEY");
    std::string env_default_model = get_env_var("DEFAULT_MODEL");
    global_config.redis_host = get_env_var("REDIS_HOST", "127.0.0.1"); 

    try {
        std::ifstream f(config_path); 
        if (f.is_open()) {
            json data = json::parse(f);
            global_config.qwen_key = env_qwen.empty() ? data["api_keys"].value("qwen", "") : env_qwen;
            global_config.openai_key = env_openai.empty() ? data["api_keys"].value("openai", "") : env_openai;
            global_config.deepseek_key = env_deepseek.empty() ? data["api_keys"].value("deepseek", "") : env_deepseek;
            global_config.default_model = env_default_model.empty() ? data["routing"].value("default_model", "qwen-turbo") : env_default_model;
        } else {
            global_config.qwen_key = env_qwen; global_config.openai_key = env_openai;
            global_config.deepseek_key = env_deepseek; global_config.default_model = env_default_model.empty() ? "qwen-turbo" : env_default_model;
        }
        trim_key(global_config.qwen_key); trim_key(global_config.openai_key); trim_key(global_config.deepseek_key);
    } catch (...) {}
}

// =========================================================================
// Redis 连接池管理
// =========================================================================
class RedisConnectionPool {
private:
    std::queue<redisContext*> pool_; 
    std::mutex mutex_; 
    std::condition_variable cond_;
    bool valid_;
public:
    RedisConnectionPool(size_t pool_size, const std::string& ip, int port) {
        struct timeval timeout = { 0, 500000 }; 
        for(size_t i=0; i<pool_size; i++) {
            redisContext* ctx = redisConnectWithTimeout(ip.c_str(), port, timeout);
            if (ctx && !ctx->err) { pool_.push(ctx); valid_ = true; } 
            else { if (ctx) redisFree(ctx); valid_ = false; break; }
        }
    }
    bool is_valid() { return valid_; }
    redisContext* getConnection() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (pool_.empty()) return nullptr;
        redisContext* ctx = pool_.front(); pool_.pop(); return ctx;
    }
    void releaseConnection(redisContext* ctx) {
        if (!ctx) return;
        std::lock_guard<std::mutex> lock(mutex_); pool_.push(ctx); cond_.notify_one();
    }
};

RedisConnectionPool* global_kvs_pool = nullptr;

// Redis 连接管理器 (RAII 模式，确保连接用后自动归还连接池)
struct ConnectionGuard {
    RedisConnectionPool* p; redisContext* c;
    ConnectionGuard(RedisConnectionPool* pool) : p(pool) { c = p ? p->getConnection() : nullptr; }
    ~ConnectionGuard() { if (p && c) p->releaseConnection(c); }
    redisContext* get() { return c; }
};

// =========================================================================
// 内部微服务调用接口 (同步调用)
// =========================================================================
// libcurl 的数据接收回调函数，将收到的 HTTP Body 存入 std::string
size_t MicroserviceWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// 调用本地 Embedding 微服务，将文本转换为特征向量
std::vector<float> generate_local_embedding(const std::string& text) {
    std::vector<float> vec; 
    if (text.empty()) return vec;

    CURL *curl = curl_easy_init();
    if (curl) {
        std::string url = "http://127.0.0.1:8001/embed";
        json payload = {{"text", text}};
        std::string payload_str = payload.dump();
        std::string response_string;

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, MicroserviceWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L); 

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK) {
            try {
                auto j = json::parse(response_string);
                if (j.contains("embedding") && j["embedding"].is_array()) {
                    auto emb_array = j["embedding"];
                    vec.resize(VECTOR_DIM, 0.0f);
                    size_t copy_len = std::min((size_t)emb_array.size(), (size_t)VECTOR_DIM);
                    for (size_t i = 0; i < copy_len; i++) {
                        if (emb_array[i].is_number()) vec[i] = emb_array[i].get<float>();
                    }
                }
            } catch(...) { vec.clear(); }
        }
    }
    return vec; 
}

// 调用本地 Rerank 微服务，计算当前查询与缓存查询的语义相似度得分
float get_rerank_score(const std::string& query, const std::string& cached_query) {
    float score = 0.0f;
    CURL *curl = curl_easy_init();
    if (curl) {
        std::string url = "http://127.0.0.1:8001/rerank";
        json payload = {{"query", query}, {"cached_query", cached_query}};
        std::string payload_str = payload.dump();
        std::string response_string;

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, MicroserviceWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2500L); 

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK) {
            try {
                auto j = json::parse(response_string);
                if (j.contains("score")) score = j["score"].get<float>();
            } catch(...) {}
        }
    }
    return score;
}

// 调用 Tokenizer 微服务，计算请求消息对应的 Token 数量，用于计费及风控
int get_token_count_from_microservice(const std::string& model, const std::string& messages_json_str) {
    int token_count = 0;
    CURL *curl = curl_easy_init();
    if (curl) {
        std::string url = "http://127.0.0.1:8001/tokenizer/" + model;
        std::string payload = "{\"messages\": " + messages_json_str + "}";
        std::string response_string;

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, MicroserviceWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L); 

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK) {
            try {
                auto j = json::parse(response_string);
                if (j.contains("num_tokens")) token_count = j["num_tokens"].get<int>();
            } catch(...) {}
        }
    }
    return token_count;
}

// =========================================================================
// 语义逻辑校验模块
// =========================================================================
// 检测两句话之间是否存在否定语义冲突，降低缓存误命中率
bool has_negation_conflict(const std::string& q1, const std::string& q2) {
    std::vector<std::string> neg_words = {
        "不支持", "不能", "不可以", "禁止", "不要", "没有", "无法", "并未", "不是"
    };
    for (const auto& word : neg_words) {
        bool q1_has = (q1.find(word) != std::string::npos);
        bool q2_has = (q2.find(word) != std::string::npos);
        // 若一方包含该词而另一方不包含，则判定为冲突
        if (q1_has != q2_has) return true; 
    }
    return false;
}

// 检测两句话是否为简短的恶意反转（例如长度相近但语义翻转）
bool is_suspicious_reversal(const std::string& q1, const std::string& q2) {
    int diff = std::abs((int)q1.length() - (int)q2.length());
    if (diff <= 3 && q1 != q2) return true;
    return false;
}

// =========================================================================
// io_uring 异步状态机定义
// =========================================================================
enum { STATE_ACCEPT, STATE_READ_REQUEST };
struct base_ctx { int ctx_type; };

// 网络连接上下文对象，贯穿单次请求的生命周期
struct conn_info : public base_ctx {
    int fd;                             // 客户端 socket fd
    int type;                           // 状态机状态 (ACCEPT / READ)
    char tcp_buf[TCP_BUF_SIZE];         // TCP 读缓冲区
    int read_offset;                    // 当前读取偏移量
    
    CURL *easy_handle;                  // 绑定的 curl 异步句柄
    struct curl_slist *headers;         // HTTP 请求头信息
    
    std::string pure_user_query;        // 解析后的纯用户提问文本
    std::string llm_full_answer;        // 缓存的大模型完整回复内容
    std::string sse_buffer;             // SSE 流数据解析缓冲
    std::string req_model;              // 请求对应的模型名称
    
    struct sockaddr_in client_addr; 
    socklen_t client_len;

    conn_info() { 
        ctx_type = 0; headers = nullptr; easy_handle = nullptr; 
        read_offset = 0; client_len = sizeof(client_addr); 
    }
};

struct io_uring global_ring;

// 获取可用的 io_uring 提交队列项
struct io_uring_sqe* get_safe_sqe() {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&global_ring);
    if (!sqe) { 
        io_uring_submit(&global_ring); 
        sqe = io_uring_get_sqe(&global_ring); 
    }
    return sqe;
}

// 同步阻塞方式向客户端套接字写入数据
void sync_write_response(int client_fd, const std::string& data) {
    if (data.empty()) return;
    size_t total_sent = 0;
    while (total_sent < data.length()) {
        ssize_t sent = send(client_fd, data.c_str() + total_sent, data.length() - total_sent, MSG_NOSIGNAL);
        if (sent < 0) { 
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; } 
            break; 
        }
        total_sent += sent;
    }
}

// =========================================================================
// 大模型请求分发与响应流处理
// =========================================================================
// 处理大模型返回的流式数据，将数据直接转发给客户端并在内存中拼装完整答案
size_t API_StreamWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    conn_info *conn = (conn_info *)userdata;
    size_t realsize = size * nmemb;
    
    // 直接向客户端输出底层接收到的 SSE 切片
    sync_write_response(conn->fd, std::string(ptr, realsize));

    // 将碎片存入缓冲，按行解析提取正文内容，供后续写入缓存使用
    conn->sse_buffer.append(ptr, realsize);
    size_t pos;
    while ((pos = conn->sse_buffer.find('\n')) != std::string::npos) {
        std::string line = conn->sse_buffer.substr(0, pos);
        conn->sse_buffer.erase(0, pos + 1); 
        
        if (!line.empty() && line.back() == '\r') line.pop_back(); 

        if (line.find("data: ") == 0) {
            std::string payload = line.substr(6);
            if (payload.find("[DONE]") == std::string::npos && payload.length() > 2) {
                try {
                    auto j = json::parse(payload);
                    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                        auto delta = j["choices"][0].value("delta", json::object());
                        if (delta.contains("content") && delta["content"].is_string()) {
                            conn->llm_full_answer += delta["content"].get<std::string>();
                        }
                    }
                } catch(...) { }
            }
        }
    }
    return realsize;
}

// 核心处理函数：解析客户端请求，执行缓存检索，未命中则发起远端请求
void initiate_curl_api_request(CURLM *multi, conn_info *conn, const std::string& body) {
    std::string model = global_config.default_model;
    std::string final_body = body;
    int prompt_tokens = 0;

    // 1. 解析请求体，获取模型名称和用户查询文本，并进行 Token 长度安全校验
    try {
        json p = json::parse(body);
        if (p.contains("model")) model = p["model"].get<std::string>();
        
        if (p.contains("messages") && p["messages"].is_array() && !p["messages"].empty()) {
            prompt_tokens = get_token_count_from_microservice(model, p["messages"].dump());
            
            if (prompt_tokens > 16000) {
                std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
                sync_write_response(conn->fd, headers);
                std::string err_msg = "data: {\"error\": \"Prompt is too long! Max limit is 16000 tokens.\"}\n\ndata: [DONE]\n\n";
                sync_write_response(conn->fd, err_msg);
                close(conn->fd); delete conn; return;
            }

            auto last_msg = p["messages"].back();
            if (last_msg.contains("content")) {
                conn->pure_user_query = last_msg["content"].get<std::string>();
            }
        }
        p["stream"] = true; // 强制启用大模型的流式输出模式
        final_body = p.dump(); 
    } catch(...) {}

    conn->req_model = model;
    bool hit = false; std::string ans = "";

    // 记录对应模型的调用统计
    if (model.find("qwen") != std::string::npos) { metric_qwen++; std::cout << "[8085] 模型调用 [qwen-turbo] -> " << metric_qwen.load() << std::endl; }
    else if (model.find("gpt") != std::string::npos) { metric_gpt4o++; std::cout << "[8085] 模型调用 [gpt-4o] -> " << metric_gpt4o.load() << std::endl; }
    else if (model.find("deepseek") != std::string::npos) { metric_deepseek++; std::cout << "[8085] 模型调用 [deepseek-chat] -> " << metric_deepseek.load() << std::endl; }
    else { metric_qwen++; }
    
    // 2. 缓存检索阶段
    if (global_kvs_pool && global_kvs_pool->is_valid() && !conn->pure_user_query.empty()) {
        ConnectionGuard g(global_kvs_pool);
        if (g.get()) {
            std::vector<float> q_vec = generate_local_embedding(conn->pure_user_query);
            
            if (!q_vec.empty()) {
                std::string bin_vec((char*)q_vec.data(), q_vec.size() * sizeof(float));
                // 使用向量进行粗筛查找相似问题
                redisReply *r = (redisReply *)redisCommand(g.get(), "VSEARCH %b 5", bin_vec.data(), (size_t)bin_vec.size());
                
                if (r && r->type == REDIS_REPLY_ARRAY && r->elements >= 2) {
                    std::string matched_ans_key = "";
                    float distance = 1.0f;

                    // 遍历结果，确保命中的缓存属于相同模型
                    for (size_t i = 0; i < r->elements; i += 2) {
                        std::string key_candidate = r->element[i]->str;
                        std::string expected_prefix = "idx:" + conn->req_model + ":";
    
                        if (key_candidate.compare(0, expected_prefix.length(), expected_prefix) == 0) {
                            matched_ans_key = key_candidate;
                            distance = std::stof(r->element[i+1]->str);
                            std::cout << "[8085] 命中合法缓存: " << matched_ans_key << std::endl;
                            break; 
                        } else {
                            std::cout << "[8085] 隔离异构模型缓存: " << key_candidate << std::endl;
                        }
                    }

                    if (!matched_ans_key.empty() && distance < 0.80f) {
                        redisReply *ans_r = (redisReply *)redisCommand(g.get(), "HGET %s", matched_ans_key.c_str());
                        std::string q_key = matched_ans_key + "_q"; 
                        redisReply *q_r = (redisReply *)redisCommand(g.get(), "HGET %s", q_key.c_str());

                        if (ans_r && ans_r->type == REDIS_REPLY_STRING && q_r && q_r->type == REDIS_REPLY_STRING) {
                            std::string cached_ans = ans_r->str;
                            std::string cached_query = q_r->str;

                            bool sentinel_pass = true;
                            
                            // 逻辑校验：拦截包含否定冲突的内容，以及可疑的语义翻转
                            if (has_negation_conflict(conn->pure_user_query, cached_query)) sentinel_pass = false;
                            if (sentinel_pass && distance < 0.03f && conn->pure_user_query != cached_query) sentinel_pass = false; 

                            // 根据向量距离调整重排打分的阈值要求
                            float dynamic_threshold = 0.95f - (0.1f * distance);
                            if (sentinel_pass && distance >= 0.03f && distance < 0.1f && is_suspicious_reversal(conn->pure_user_query, cached_query)) {
                                dynamic_threshold = 0.99f;
                            }

                            // 依赖交叉编码器模型进行最终分数评定
                            if (sentinel_pass) {
                                float logic_score = get_rerank_score(conn->pure_user_query, cached_query);
                                if (logic_score > dynamic_threshold) {
                                    hit = true;
                                    ans = cached_ans;
                                    
                                    metric_hits++;
                                    std::cout << "[8085] 缓存命中总量 -> " << metric_hits.load() << std::endl;
                                }
                            }
                        }
                        if (ans_r) freeReplyObject(ans_r);
                        if (q_r) freeReplyObject(q_r);
                    }
                }
                if (r) freeReplyObject(r);
            }
        }
    }
    
    // 3. 缓存命中处理：伪造流式输出返回缓存数据并预估 Token
    if (hit) {
        std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n"
                              "X-Inazuma-Cache: HIT\r\n\r\n";
        sync_write_response(conn->fd, headers);

        ans = "[缓存直达]\n\n" + ans;

        int estimated_ans_tokens = ans.length() / 3;
        int discount_tokens = (prompt_tokens + estimated_ans_tokens) * 0.1; 
        if (discount_tokens < 1) discount_tokens = 1;

        try {
            json s = {
                {"id", "chatcmpl-semantic-cache"}, {"object", "chat.completion.chunk"}, {"model", model},
                {"choices", {{ {"delta", {{"content", ans}}}, {"index", 0}, {"finish_reason", nullptr} }}}
            };
            std::string sse_data = "data: " + s.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
            sync_write_response(conn->fd, sse_data);
            
            json sf = {
                {"id", "chatcmpl-semantic-cache"}, {"object", "chat.completion.chunk"}, {"model", model},
                {"choices", {{ {"delta", json::object()}, {"index", 0}, {"finish_reason", "stop"} }}},
                {"usage", {
                    {"prompt_tokens", discount_tokens},
                    {"completion_tokens", 0},
                    {"total_tokens", discount_tokens}
                }}
            };
            sync_write_response(conn->fd, "data: " + sf.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\ndata: [DONE]\n\n");

        } catch (...) { }
        
        close(conn->fd); delete conn; return;
    }

    // 4. 穿透回源处理：组装 API 请求配置，挂载至 curl_multi 事件循环
    metric_misses++;
    std::cout << "[8085] 穿透回源总量 -> " << metric_misses.load() << std::endl;

    std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    sync_write_response(conn->fd, headers);

    std::string url, key;
    if (model.find("deepseek") != std::string::npos) {
        url = "https://api.deepseek.com/chat/completions";
        key = global_config.deepseek_key;
    } else if (model.find("openai") != std::string::npos || model.find("gpt") != std::string::npos) {
        url = "https://api.openai.com/v1/chat/completions";
        key = global_config.openai_key;
    } else {
        url = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"; 
        key = global_config.qwen_key;
    }

    CURL *e = curl_easy_init();
    struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
    h = curl_slist_append(h, ("Authorization: Bearer " + key).c_str());
    conn->headers = h;
    curl_easy_setopt(e, CURLOPT_URL, url.c_str());
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, API_StreamWriteCallback);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, conn);
    curl_easy_setopt(e, CURLOPT_PRIVATE, conn);
    curl_easy_setopt(e, CURLOPT_COPYPOSTFIELDS, final_body.c_str());
    
    // 添加至异步引擎列队
    curl_multi_add_handle(multi, e);
}

// =========================================================================
// 主入口与核心事件循环
// =========================================================================
int main() {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    std::cout << "[8085] 网关引擎初始化中..." << std::endl;
    load_config("../conf/gateway.json");

    global_kvs_pool = new RedisConnectionPool(10, "127.0.0.1", 6379); 
    if (!global_kvs_pool->is_valid()) { 
        std::cerr << "[8085] 错误：无法连接 Redis" << std::endl;
        delete global_kvs_pool; global_kvs_pool = nullptr; 
    } else {
        std::cout << "[8085] 成功连接 Redis" << std::endl;
    }

    int ring_ret = io_uring_queue_init(RING_ENTRIES, &global_ring, 0);
    if (ring_ret < 0) { exit(1); }

    curl_global_init(CURL_GLOBAL_ALL);
    CURLM *multi = curl_multi_init();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { exit(1); }
    if (listen(server_fd, 4096) < 0) { exit(1); }
    
    std::cout << "[8085] 监听就绪，端口: " << PORT << std::endl;

    // 投递首个 io_uring Accept 读事件
    conn_info *accept_conn = new conn_info(); accept_conn->type = STATE_ACCEPT;
    struct io_uring_sqe *sqe = get_safe_sqe();
    io_uring_prep_accept(sqe, server_fd, (struct sockaddr *)&accept_conn->client_addr, &accept_conn->client_len, 0);
    io_uring_sqe_set_data(sqe, accept_conn);
    io_uring_submit(&global_ring);

    int running;
    
    // 主事件轮询
    while(true) {
        // [阶段一] 处理外部请求回调 (curl_multi)
        curl_multi_perform(multi, &running);
        int m_left; CURLMsg *msg = curl_multi_info_read(multi, &m_left);
        while(msg) {
            if(msg->msg == CURLMSG_DONE) {
                conn_info *c; curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &c);
                if(c) {
                    // 请求完结时执行缓存写入
                    if(!c->pure_user_query.empty() && !c->llm_full_answer.empty() && global_kvs_pool) {
                        ConnectionGuard g(global_kvs_pool);
                        if(g.get()) {
                            if (c->pure_user_query.length() > 5 && c->llm_full_answer.length() > 15) {
                                // 拼接包含模型标识的 Key
                                std::string ans_key = "idx:" + c->req_model + ":" + std::to_string(std::hash<std::string>{}(c->pure_user_query));
                                std::string q_key = ans_key + "_q";
                                std::vector<float> vec = generate_local_embedding(c->pure_user_query);
                                
                                if (!vec.empty()) {
                                    std::string bin_vec((char*)vec.data(), vec.size() * sizeof(float));
                                    
                                    redisReply *r1 = (redisReply *)redisCommand(g.get(), "VADD %s %b", ans_key.c_str(), bin_vec.data(), (size_t)bin_vec.size());
                                    if (r1) freeReplyObject(r1);
                                    
                                    redisReply *r2 = (redisReply *)redisCommand(g.get(), "HSET %s %b", ans_key.c_str(), c->llm_full_answer.c_str(), (size_t)c->llm_full_answer.length());
                                    if (r2) freeReplyObject(r2);
                                    
                                    redisReply *r3 = (redisReply *)redisCommand(g.get(), "HSET %s %b", q_key.c_str(), c->pure_user_query.c_str(), (size_t)c->pure_user_query.length());
                                    if (r3) freeReplyObject(r3);
                                }
                            }
                        }
                    }
                    if(c->headers) curl_slist_free_all(c->headers); 
                    close(c->fd); delete c;
                }
                curl_multi_remove_handle(multi, msg->easy_handle);
                curl_easy_cleanup(msg->easy_handle);
            }
            msg = curl_multi_info_read(multi, &m_left);
        }

        // [阶段二] 处理网络 I/O 读写事件 (io_uring)
        struct io_uring_cqe *cqe; struct __kernel_timespec ts = {0, 1000000}; 
        if(io_uring_wait_cqe_timeout(&global_ring, &cqe, &ts) == 0) {
            if (!cqe) continue;
            base_ctx *b = (base_ctx *)io_uring_cqe_get_data(cqe);
            if (!b) { io_uring_cqe_seen(&global_ring, cqe); continue; }

            if(b->ctx_type == 0) { 
                conn_info *c = (conn_info *)b;
                
                // 处理客户端连接请求
                if(c->type == STATE_ACCEPT) {
                    int c_fd = cqe->res;
                    if(c_fd >= 0) {
                        conn_info *nc = new conn_info(); nc->fd = c_fd; nc->type = STATE_READ_REQUEST;
                        struct io_uring_sqe *s = get_safe_sqe();
                        io_uring_prep_read(s, c_fd, nc->tcp_buf, TCP_BUF_SIZE, 0);
                        io_uring_sqe_set_data(s, nc);
                    }
                    // 重新投递 Accept 事件
                    c->client_len = sizeof(c->client_addr); 
                    struct io_uring_sqe *s = get_safe_sqe();
                    io_uring_prep_accept(s, server_fd, (struct sockaddr *)&c->client_addr, &c->client_len, 0);
                    io_uring_sqe_set_data(s, c);
                } 
                // 处理客户端数据读取请求
                else if(c->type == STATE_READ_REQUEST) {
                    if (cqe->res <= 0) { close(c->fd); delete c; } 
                    else {
                        c->read_offset += cqe->res;
                        std::string data(c->tcp_buf, c->read_offset);
                        size_t size_h_end = data.find("\r\n\r\n");
                        
                        if(size_h_end != std::string::npos) {
                            // 路由处理：输出指标信息
                            if (data.find("GET /metrics") == 0) {
                                json m = {
                                    {"hits", metric_hits.load()},
                                    {"misses", metric_misses.load()},
                                    {"qwen-turbo", metric_qwen.load()},
                                    {"gpt-4o", metric_gpt4o.load()},
                                    {"deepseek-chat", metric_deepseek.load()}
                                };
                                std::string json_str = m.dump();
                                std::string resp = "HTTP/1.1 200 OK\r\n"
                                                   "Access-Control-Allow-Origin: *\r\n"
                                                   "Content-Type: application/json\r\n"
                                                   "Content-Length: " + std::to_string(json_str.length()) + "\r\n"
                                                   "Connection: close\r\n\r\n" + json_str;
                                sync_write_response(c->fd, resp);
                                close(c->fd); delete c;
                            } 
                            // 路由处理：API 请求，校验 Content-Length 以确保 Body 接收完毕
                            else {
                                size_t cl_p = data.find("Content-Length: "); 
                                if (cl_p == std::string::npos) cl_p = data.find("content-length: "); 
                                int cl = 0;
                                if (cl_p != std::string::npos) {
                                    size_t cl_start = cl_p + 16; size_t cl_end = data.find("\r\n", cl_start);
                                    if (cl_end != std::string::npos) {
                                        try { cl = std::stoi(data.substr(cl_start, cl_end - cl_start)); } catch (...) {}
                                    }
                                }
                                
                                // 若数据完整，则向后流转处理请求
                                if((size_t)c->read_offset >= size_h_end + 4 + (size_t)cl) { 
                                    initiate_curl_api_request(multi, c, data.substr(size_h_end+4)); 
                                } 
                                // 数据不完整，继续投递读事件读取剩余内容
                                else {
                                    struct io_uring_sqe *s = get_safe_sqe();
                                    io_uring_prep_read(s, c->fd, c->tcp_buf + c->read_offset, TCP_BUF_SIZE - c->read_offset, 0);
                                    io_uring_sqe_set_data(s, c);
                                }
                            }
                        } 
                        // 请求头不完整，继续投递读事件
                        else {
                            struct io_uring_sqe *s = get_safe_sqe();
                            io_uring_prep_read(s, c->fd, c->tcp_buf + c->read_offset, TCP_BUF_SIZE - c->read_offset, 0);
                            io_uring_sqe_set_data(s, c);
                        }
                    }
                }
            }
            io_uring_cqe_seen(&global_ring, cqe);
        }
        io_uring_submit(&global_ring);
    }
    return 0; 
}