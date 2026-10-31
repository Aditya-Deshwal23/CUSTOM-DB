#ifndef RELDB_API_INTERNAL_H
#define RELDB_API_INTERNAL_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "api_server.h"
#include "crow.h"
#include "db_error.h"
#include "sql_parser.h"

namespace relational_db {

using ApiConfig = Config;

struct ApiRequestContext {
    std::string request_id;
    std::string client_ip;
    std::chrono::steady_clock::time_point started;
    bool active_counted{false};
};

class ApiMiddleware {
   public:
    struct context : ApiRequestContext {};

    ApiConfig config;
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> queries{0};
    std::atomic<uint64_t> active_requests{0};
    std::atomic<bool> draining{false};
    std::chrono::steady_clock::time_point started_at{std::chrono::steady_clock::now()};

    void before_handle(crow::request& request, crow::response& response, context& ctx);
    void after_handle(crow::request& request, crow::response& response, context& ctx);
    bool WaitForIdle(std::chrono::milliseconds timeout);
    void CompleteRequest(context& ctx);

   private:
    struct Bucket {
        double tokens{0.0};
        std::chrono::steady_clock::time_point updated;
    };

    bool ConsumeToken(const std::string& client_ip);
    std::mutex buckets_mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::mutex active_requests_mutex_;
    std::condition_variable active_requests_changed_;
    std::atomic<uint64_t> next_request_id_{1};
};

struct ApiServer::Impl {
    Impl(std::shared_ptr<BufferPoolManager> buffer_pool, std::shared_ptr<IDiskManager> disk,
         std::shared_ptr<Executor> query_executor, std::string static_path, Config service_config);

    void RegisterRoutes();

    std::shared_ptr<BufferPoolManager> bpm;
    std::shared_ptr<IDiskManager> disk_manager;
    std::shared_ptr<Executor> executor;
    ApiConfig config;
    crow::App<ApiMiddleware> app;
    std::unordered_map<std::string, std::string> static_files;
    std::atomic<bool> recovering{false};
    std::atomic<bool> draining{false};
    std::mutex stats_window_mutex;
    uint64_t previous_stats_hits{0};
    uint64_t previous_stats_misses{0};
    std::chrono::steady_clock::time_point previous_stats_at{};
};

crow::response JsonResponse(int status, const crow::json::wvalue& value);
crow::response ErrorResponse(int status, std::string code, std::string message,
                             const std::string& request_id, size_t line = 0, size_t column = 0,
                             std::string hint = {});
void ApplyResponseHeaders(crow::response& response);
std::string BindParameters(const std::string& sql, const crow::json::rvalue& params);
std::string SqlStatementName(SqlStatementKind kind);
bool ApiKeyMatches(const std::string& expected, const std::string& supplied);

}  // namespace relational_db

#endif
