/**
 * ==============================================================================
 * Copyright (c) 2026 Nexus_Yao. All rights reserved.
 *
 * Project:      InazumaChatServer
 * File:         chat_server.cpp
 * Description:  聊天服务端模块。
 * 提供用户注册、登录鉴权（JWT）接口，并作为大模型请求的
 * 计费与代理服务，实现基于 Token 消耗的额度扣减与缓存计费策略。
 * ==============================================================================
 */

#include <iostream>
#include <string>
#include <chrono>
#include <vector>
#include <thread>
#include <memory>
#include <cstdlib>
#include <mysql/mysql.h>
#include "httplib.h"
#include "json.hpp"

#define PICOJSON_USE_INT64 
#include <picojson.h>
#include "jwt-cpp/jwt.h"

using json = nlohmann::json;

// =========================================================================
// 基础工具模块
// =========================================================================

// 获取环境变量，若未设置则返回预设的默认值
std::string get_env_var(const char* key, const std::string& default_val) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : default_val;
}

// 建立 MySQL 数据库连接
// 遍历备选地址列表以适配不同的部署环境（物理机、Docker 网络等），配置信息通过环境变量注入
MYSQL* connect_mysql() {
    const char* hosts[] = {"192.168.113.128", "172.17.0.1", "172.18.0.1", "host.docker.internal", "127.0.0.1"};
    std::string last_err;
    
    std::string db_user = get_env_var("DB_USER", "inazuma_user");
    std::string db_pass = get_env_var("DB_PASSWORD", "Inazuma_2026!");

    for (const char* host : hosts) {
        MYSQL *conn = mysql_init(NULL);
        if (mysql_real_connect(conn, host, db_user.c_str(), db_pass.c_str(), "inazuma_chat", 3306, NULL, 0)) return conn;
        last_err = mysql_error(conn);
        mysql_close(conn);
    }
    std::cout << "[8081-ERROR] MySQL 连接失败，最后一次错误信息：" << last_err << std::endl;
    return NULL;
}

// 配置全局跨域资源共享 (CORS) 响应头
void set_cors_headers(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

// =========================================================================
// 主服务逻辑
// =========================================================================
int main() {
    std::cout << "[8081] Inazuma Chat Server 服务启动..." << std::endl;
    
    // 加载 JWT 签名密钥
    std::string jwt_secret = get_env_var("JWT_SECRET", "inazuma_ultra_secret_key_2026_!@#");
    
    httplib::Server svr;

    // 注册跨域预检 (OPTIONS) 路由
    svr.Options("/api/register", [](const httplib::Request& req, httplib::Response& res) { set_cors_headers(res); res.status = 204; });
    svr.Options("/api/login", [](const httplib::Request& req, httplib::Response& res) { set_cors_headers(res); res.status = 204; });
    svr.Options("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) { set_cors_headers(res); res.status = 204; });

    // 处理用户注册请求
    // 执行 JSON 解析、空值校验、防 SQL 注入转义，并分配初始调用额度
    svr.Post("/api/register", [](const httplib::Request& req, httplib::Response& res) {
        set_cors_headers(res);
        try {
            auto req_body = json::parse(req.body);
            std::string username = req_body.value("username", ""), password = req_body.value("password", "");
            if (username.empty() || password.empty()) { res.set_content(json{{"code", 400}, {"message", "账号或密码为空"}}.dump(), "application/json"); return; }
            
            MYSQL *conn = connect_mysql();
            if (!conn) { res.status = 500; res.set_content(json{{"code", 500}, {"message", "数据库错误"}}.dump(), "application/json"); return; }
            
            std::vector<char> escaped_user(username.length() * 2 + 1), escaped_pass(password.length() * 2 + 1);
            mysql_real_escape_string(conn, escaped_user.data(), username.c_str(), username.length());
            mysql_real_escape_string(conn, escaped_pass.data(), password.c_str(), password.length());
            
            std::string query = "INSERT INTO users (username, password_hash, quota) VALUES ('" + std::string(escaped_user.data()) + "', '" + std::string(escaped_pass.data()) + "', 100000)";
            if (mysql_query(conn, query.c_str())) res.set_content(json{{"code", 409}, {"message", "注册失败或账号已存在"}}.dump(), "application/json");
            else res.set_content(json{{"code", 200}, {"message", "注册成功！"}, {"data", {{"username", username}, {"quota", 100000}}}}.dump(), "application/json");
            mysql_close(conn);
        } catch (...) { res.set_content(json{{"code", 400}, {"message", "无效 JSON"}}.dump(), "application/json"); }
    });

    // 处理用户登录请求
    // 验证用户凭证，验证通过后签发包含用户信息的 JWT 令牌，有效期设为 168 小时
    svr.Post("/api/login", [jwt_secret](const httplib::Request& req, httplib::Response& res) {
        set_cors_headers(res);
        try {
            auto req_body = json::parse(req.body);
            std::string username = req_body.value("username", ""), password = req_body.value("password", "");
            
            MYSQL *conn = connect_mysql();
            if (!conn) { res.set_content(json{{"code", 500}, {"message", "数据库错误"}}.dump(), "application/json"); return; }
            
            std::vector<char> escaped_user(username.length() * 2 + 1);
            mysql_real_escape_string(conn, escaped_user.data(), username.c_str(), username.length());
            
            std::string query = "SELECT id, password_hash, quota FROM users WHERE username = '" + std::string(escaped_user.data()) + "'";
            if (mysql_query(conn, query.c_str()) == 0) {
                MYSQL_RES *result = mysql_store_result(conn);
                if (result && mysql_num_rows(result) > 0) {
                    MYSQL_ROW row = mysql_fetch_row(result);
                    if (password == row[1]) {
                        auto token = jwt::create().set_issuer("inazuma_auth_center").set_type("JWT").set_issued_at(std::chrono::system_clock::now())
                            .set_expires_at(std::chrono::system_clock::now() + std::chrono::hours(168)).set_payload_claim("user_id", jwt::claim(std::string(row[0])))
                            .set_payload_claim("username", jwt::claim(username)).sign(jwt::algorithm::hs256{jwt_secret});
                        res.set_content(json{{"code", 200}, {"message", "登录成功！"}, {"data", {{"token", token}, {"username", username}, {"quota", std::stoi(row[2])}}}}.dump(), "application/json");
                    } else res.set_content(json{{"code", 401}, {"message", "账号不存在或密码错误！"}}.dump(), "application/json");
                } else res.set_content(json{{"code", 401}, {"message", "账号不存在或密码错误！"}}.dump(), "application/json");
                if (result) mysql_free_result(result);
            }
            mysql_close(conn);
        } catch (...) { res.set_content(json{{"code", 400}, {"message", "无效 JSON"}}.dump(), "application/json"); }
    });

    // 处理大模型对话的代理及计费请求
    svr.Post("/v1/chat/completions", [jwt_secret](const httplib::Request& req, httplib::Response& res) {
        set_cors_headers(res);
        std::string auth_header = req.get_header_value("Authorization");
        if (auth_header.empty() || auth_header.substr(0, 7) != "Bearer ") {
            res.status = 401; res.set_content(json{{"code", 401}, {"message", "未提供身份令牌"}}.dump(), "application/json"); return;
        }

        std::string token = auth_header.substr(7);
        while (!token.empty() && (token.back() == '\r' || token.back() == '\n' || token.back() == ' ')) token.pop_back();

        std::string username;
        
        // 鉴权解析：支持直接查询数据库的 API Key 模式，以及本地验签的 JWT 模式
        try {
            if (token.substr(0, 3) == "sk-") {
                MYSQL *conn = connect_mysql();
                if (conn) {
                    std::vector<char> escaped_token(token.length() * 2 + 1);
                    mysql_real_escape_string(conn, escaped_token.data(), token.c_str(), token.length());
                    std::string sql = "SELECT username FROM users WHERE api_key = '" + std::string(escaped_token.data()) + "'";
                    if (mysql_query(conn, sql.c_str()) == 0) {
                        MYSQL_RES *res_db = mysql_store_result(conn);
                        if (res_db && mysql_num_rows(res_db) > 0) username = mysql_fetch_row(res_db)[0];
                        if(res_db) mysql_free_result(res_db);
                    }
                    mysql_close(conn);
                }
            } else {
                auto decoded = jwt::decode(token);
                auto verifier = jwt::verify().allow_algorithm(jwt::algorithm::hs256{jwt_secret}).with_issuer("inazuma_auth_center");
                verifier.verify(decoded); 
                username = decoded.get_payload_claim("username").as_string();
            }
        } catch (...) {}

        if (username.empty()) {
            res.status = 401; res.set_content(json{{"code", 401}, {"message", "Token 无效或已过期"}}.dump(), "application/json"); return;
        }

        // 额度前置校验
        MYSQL *conn = connect_mysql();
        if (!conn) { res.status = 500; res.set_content(json{{"code", 500}, {"message", "计费中心失联"}}.dump(), "application/json"); return; }
        
        std::vector<char> escaped_user(username.length() * 2 + 1);
        mysql_real_escape_string(conn, escaped_user.data(), username.c_str(), username.length());
        std::string safe_username = std::string(escaped_user.data());
        
        std::string check_sql = "SELECT quota FROM users WHERE username = '" + safe_username + "'";
        mysql_query(conn, check_sql.c_str());
        MYSQL_RES *res_db = mysql_store_result(conn);
        if (!res_db || mysql_num_rows(res_db) == 0) {
            mysql_close(conn); res.status = 403; return;
        }
        int current_quota = std::stoi(mysql_fetch_row(res_db)[0]);
        mysql_free_result(res_db);
        mysql_close(conn);

        if (current_quota <= 0) {
            res.status = 403; res.set_content(json{{"error", {{"message", "Token 额度耗尽"}, {"type", "insufficient_quota"}}}}.dump(), "application/json"); 
            return;
        }
        
        std::cout << "\n[8081] --------------------------------" << std::endl;
        std::cout << "[8081] 鉴权通过 [" << username << "]，建立会话通道" << std::endl;

        std::string body_str = req.body;
        
        // 修改请求体，注入 stream_options 参数以确保后端返回最终的 Token 使用量
        try {
            json body_j = json::parse(body_str);
            body_j["stream_options"] = {{"include_usage", true}};
            body_str = body_j.dump();
        } catch(...) {}

        std::shared_ptr<int> real_tokens_used = std::make_shared<int>(0);
        std::shared_ptr<std::string> stream_tail = std::make_shared<std::string>();
        std::shared_ptr<int> total_bytes = std::make_shared<int>(body_str.length());
        std::shared_ptr<bool> is_cache_hit = std::make_shared<bool>(false);

        // 设置分块传输机制，向上游网关发起代理请求
        res.set_chunked_content_provider("text/event-stream",
            [body_str, real_tokens_used, stream_tail, total_bytes, safe_username, is_cache_hit](size_t offset, httplib::DataSink &sink) {
                if (offset > 0) return false; 
                
                httplib::Client gateway_cli("127.0.0.1", 8085);
                gateway_cli.set_read_timeout(120);
                
                auto api_res = gateway_cli.Post(
                    "/v1/chat/completions",
                    httplib::Headers(),
                    body_str, 
                    "application/json",
                    [&sink, stream_tail, total_bytes, is_cache_hit](const char *data, size_t data_length) {
                        std::string payload(data, data_length);
                        *total_bytes += data_length;
                        
                        // 识别底层返回的缓存命中系统特征符
                        if (payload.find("\"chatcmpl-cache\"") != std::string::npos) {
                            *is_cache_hit = true;
                        }

                        // 保留流数据的最后 1024 字节用于最终提取 usage 统计
                        stream_tail->append(data, data_length);
                        if (stream_tail->length() > 2048) {
                            *stream_tail = stream_tail->substr(stream_tail->length() - 1024);
                        }

                        // 错误拦截：若返回内容非标准流数据而是包含错误信息的 JSON，包装为 SSE 错误流返回
                        if (payload.find("data: ") == std::string::npos && payload.find("{") != std::string::npos && payload.find("\"error\"") != std::string::npos) {
                            std::string safe_payload = payload;
                            size_t pos = 0;
                            while ((pos = safe_payload.find("\"", pos)) != std::string::npos) { safe_payload.replace(pos, 1, "\\\""); pos += 2; }
                            std::string sse_error = "data: {\"choices\": [{\"delta\": {\"content\": \"\\n\\n[底层调用异常]:\\n" + safe_payload + "\"}}]}\n\n";
                            sink.write(sse_error.c_str(), sse_error.length());
                            return true;
                        }

                        sink.write(data, data_length);
                        return true; 
                    }
                );
                
                // 解析流末尾记录的实际 token 消耗量
                size_t token_pos = stream_tail->find("\"total_tokens\":");
                if (token_pos != std::string::npos) {
                    size_t num_start = token_pos + 15;
                    while (num_start < stream_tail->length() && (*stream_tail)[num_start] == ' ') num_start++;
                    size_t num_end = stream_tail->find_first_not_of("0123456789", num_start);
                    if (num_end == std::string::npos) num_end = stream_tail->length();
                    try { *real_tokens_used = std::stoi(stream_tail->substr(num_start, num_end - num_start)); } catch(...) {}
                }

                int deduct_amount = 0;
                
                // 计费计算与结算逻辑
                // 策略 1: 缓存命中计费
                if (*is_cache_hit) {
                    deduct_amount = 1; 
                    std::cout << "[8081] 命中语义缓存，扣除 [" << safe_username << "] 1 Token" << std::endl;
                } 
                // 策略 2: 官方响应正常计费
                else if (*real_tokens_used > 0) {
                    deduct_amount = *real_tokens_used;
                    std::cout << "[8081] 接口调用结算，扣除 [" << safe_username << "] " << deduct_amount << " Tokens" << std::endl;
                } 
                // 策略 3: 无响应统计时的估算计费
                else {
                    deduct_amount = (*total_bytes) / 4;
                    if (deduct_amount < 10) deduct_amount = 10;
                    std::cout << "[8081] 调用异常结算估算，扣除 [" << safe_username << "] " << deduct_amount << " Tokens" << std::endl;
                }

                // 更新数据库中的用户剩余额度
                if (deduct_amount > 0) {
                    MYSQL *conn_settle = connect_mysql();
                    if (conn_settle) {
                        std::string settle_sql = "UPDATE users SET quota = quota - " + std::to_string(deduct_amount) + " WHERE username = '" + safe_username + "'";
                        mysql_query(conn_settle, settle_sql.c_str());
                        mysql_close(conn_settle);
                    }
                }
                
                sink.done();
                return true; 
            }
        );
    });
    
    svr.listen("0.0.0.0", 8081);
    return 0;
}