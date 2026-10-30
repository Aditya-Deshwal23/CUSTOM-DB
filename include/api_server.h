#ifndef API_SERVER_H
#define API_SERVER_H

#include "executor.h"
#include "disk_manager.h"
#include "buffer_pool_manager.h"
#include <memory>
#include <string>
#include <atomic>
#include <fstream>
#include <sstream>
#include <mutex>
#include <future>
#include <utility>
#include "crow.h"
#include "crow/middlewares/cors.h"

namespace relational_db {

class ApiServer {
public:
    ApiServer(std::shared_ptr<BufferPoolManager> bpm,
              std::shared_ptr<DiskManager> disk_manager,
              std::shared_ptr<Executor> executor,
              std::string dashboard_path = "dashboard")
        : bpm_(bpm),
          disk_manager_(disk_manager),
          executor_(executor),
          dashboard_path_(std::move(dashboard_path)) {
        
        // Step 2: Bulletproof CORS configuration
        auto& cors = app_.get_middleware<crow::CORSHandler>();
        cors.global()
            .origin("*")
            .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST);
    }

    void SetMetrics(int query_rate, int avg_latency, int error_count) {
        query_rate_ = query_rate;
        avg_latency_ = avg_latency;
        error_count_ = error_count;
    }

    void Run(int port = 8080) {
        CROW_ROUTE(app_, "/")
        ([this](){
            return ServeStaticFile("index.html", "text/html");
        });

        CROW_ROUTE(app_, "/index.css")
        ([this](){
            return ServeStaticFile("index.css", "text/css");
        });

        CROW_ROUTE(app_, "/index.js")
        ([this](){
            return ServeStaticFile("index.js", "application/javascript");
        });

        CROW_ROUTE(app_, "/health")
        ([](){
            crow::json::wvalue res;
            res["status"] = "ok";
            return crow::response(res);
        });

        CROW_ROUTE(app_, "/api/metrics")
        ([this]() {
            crow::json::wvalue x;
            x["cache_hit_rate"] = cache_hit_rate_.load();
            x["query_throughput"] = query_rate_.load();
            x["avg_latency"] = avg_latency_.load();
            x["error_count"] = error_count_.load();
            x["source"] = "synthetic_workload";
            return crow::response(x);
        });

        CROW_ROUTE(app_, "/api/query").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            auto x = crow::json::load(req.body);
            if (!x || !x.has("query") || x["query"].t() != crow::json::type::String) {
                return crow::response(400);
            }
            std::string query = x["query"].s();
            
            // Execute query via engine
            std::string result = executor_->ExecuteQuery(query);
            
            crow::json::wvalue res;
            res["status"] = "success";
            res["result"] = result;
            return crow::response(res);
        });

        app_.bindaddr("0.0.0.0").port(port).multithreaded().run();
    }

    void SetCacheHitRate(int rate) { cache_hit_rate_ = rate; }
    void Stop() { app_.stop(); }

private:
    crow::response ServeStaticFile(const std::string& file_name, const std::string& content_type) const {
        const std::string path = dashboard_path_ + "/" + file_name;
        std::ifstream file(path);
        if (!file.is_open()) {
            return crow::response(404, file_name + " not found");
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        auto res = crow::response(buffer.str());
        res.add_header("Content-Type", content_type);
        return res;
    }

    std::shared_ptr<BufferPoolManager> bpm_;
    std::shared_ptr<DiskManager> disk_manager_;
    std::shared_ptr<Executor> executor_;
    crow::App<crow::CORSHandler> app_;
    std::string dashboard_path_;

    std::atomic<int> cache_hit_rate_{0};
    std::atomic<int> query_rate_{0};
    std::atomic<int> avg_latency_{0};
    std::atomic<int> error_count_{0};
};

} // namespace relational_db

#endif // API_SERVER_H
