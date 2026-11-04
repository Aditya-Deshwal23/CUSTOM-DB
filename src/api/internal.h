#ifndef RELDB_API_INTERNAL_H
#define RELDB_API_INTERNAL_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "api_server.h"
#include "concurrency.h"
#include "crow.h"
#include "db_error.h"
#include "sql_parser.h"

namespace relational_db {

using ApiConfig = Config;
constexpr size_t kHistorySeriesCount = 41;

struct ApiHistorySample {
    uint64_t ts_ms{0};
    std::array<uint64_t, kHistorySeriesCount> values{};
};

struct SlowQuerySample {
    uint64_t ts_ms{0};
    uint64_t elapsed_us{0};
    std::string statement;
    std::string sql_preview;
    std::string plan_summary;
    std::string request_id;
};

struct ActiveQuery {
    std::string request_id;
    std::string sql_preview;
    std::chrono::steady_clock::time_point started;
    std::shared_ptr<CancellationToken> cancellation;
};

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

    bool ConsumeToken(const std::string& client_ip, bool inspect = false);
    std::mutex buckets_mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::mutex active_requests_mutex_;
    std::condition_variable active_requests_changed_;
    std::atomic<uint64_t> next_request_id_{1};
};

struct ApiServer::Impl {
    Impl(std::shared_ptr<BufferPoolManager> buffer_pool, std::shared_ptr<IDiskManager> disk,
         std::shared_ptr<Executor> query_executor, std::string static_path, Config service_config);
    ~Impl();

    void RegisterRoutes();
    void RegisterInspectorRoutes();
    void StartMetricsSampler();
    void SampleMetrics();

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
    std::mutex history_mutex;
    std::condition_variable history_changed;
    std::deque<ApiHistorySample> history;
    std::atomic<bool> stop_sampler{false};
    std::thread sampler_thread;
    std::mutex slow_queries_mutex;
    std::deque<SlowQuerySample> slow_queries;
    std::mutex active_queries_mutex;
    std::unordered_map<std::string, ActiveQuery> active_queries;
};

crow::response JsonResponse(int status, const crow::json::wvalue& value);
crow::response ErrorResponse(int status, std::string code, std::string message,
                             const std::string& request_id, size_t line = 0, size_t column = 0,
                             std::string hint = {});
void ApplyResponseHeaders(crow::response& response, bool trusted_https);
std::string BindParameters(const std::string& sql, const crow::json::rvalue& params);
std::string SqlStatementName(SqlStatementKind kind);
bool ApiKeyMatches(const std::string& expected, const std::string& supplied);
crow::response HandleBatch(ApiServer::Impl& server, const crow::request& request);

}  // namespace relational_db

#endif
