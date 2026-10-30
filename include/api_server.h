#ifndef API_SERVER_H
#define API_SERVER_H

#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "buffer_pool_manager.h"
#include "crow.h"
#include "crow/middlewares/cors.h"
#include "disk_manager.h"
#include "executor.h"

namespace relational_db {

class ApiServer {
public:
 ApiServer(std::shared_ptr<BufferPoolManager> bpm, std::shared_ptr<IDiskManager> disk_manager,
           std::shared_ptr<Executor> executor, std::string dashboard_path = "dashboard")
     : bpm_(bpm),
       disk_manager_(disk_manager),
       executor_(executor),
       dashboard_path_(std::move(dashboard_path)) {
     // Step 2: Bulletproof CORS configuration
     auto& cors = app_.get_middleware<crow::CORSHandler>();
     cors.global().origin("*").methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST);
 }

    void SetMetrics(int query_rate, int avg_latency, int error_count) {
        query_rate_ = query_rate;
        avg_latency_ = avg_latency;
        error_count_ = error_count;
    }

    void Run(int port = 8080) {
        CROW_ROUTE(app_, "/")
        ([this](){
            auto response = ServeStaticFile("index.html", "text/html");
            CheckForLeakedPins();
            return response;
        });

        CROW_ROUTE(app_, "/index.css")
        ([this](){
            auto response = ServeStaticFile("index.css", "text/css");
            CheckForLeakedPins();
            return response;
        });

        CROW_ROUTE(app_, "/index.js")
        ([this](){
            auto response = ServeStaticFile("index.js", "application/javascript");
            CheckForLeakedPins();
            return response;
        });

        CROW_ROUTE(app_, "/health")
        ([this](){
            crow::json::wvalue res;
            res["status"] = "ok";
            CheckForLeakedPins();
            return crow::response(res);
        });

        CROW_ROUTE(app_, "/api/buffer_pool")
        ([this]() {
            const BufferPoolStats stats = bpm_->SnapshotStats();
            crow::json::wvalue response;
            response["pinned_frames"] = bpm_->pinned_frames();
            response["hits"] = stats.hits;
            response["misses"] = stats.misses;
            response["evictions"] = stats.evictions;
            response["dirty_write_backs"] = stats.dirty_write_backs;
            response["waits"] = stats.waits;
            crow::json::wvalue::list frames;
            for (const FrameInfo& info : bpm_->Snapshot()) {
                crow::json::wvalue frame;
                frame["frame_id"] = info.frame_id;
                frame["page_id"] = info.page_id;
                frame["pin_count"] = info.pin_count;
                frame["dirty"] = info.dirty;
                frame["ref_bit"] = info.ref_bit;
                frame["clock_hand"] = info.clock_hand;
                frames.push_back(std::move(frame));
            }
            response["frames"] = std::move(frames);
            CheckForLeakedPins();
            return crow::response(response);
        });

        CROW_ROUTE(app_, "/api/metrics")
        ([this]() {
            crow::json::wvalue x;
            x["cache_hit_rate"] = cache_hit_rate_.load();
            x["query_throughput"] = query_rate_.load();
            x["avg_latency"] = avg_latency_.load();
            x["error_count"] = error_count_.load();
            x["source"] = "synthetic_workload";
            CheckForLeakedPins();
            return crow::response(x);
        });

        CROW_ROUTE(app_, "/api/query").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            auto x = crow::json::load(req.body);
            if (!x || !x.has("query") || x["query"].t() != crow::json::type::String) {
                CheckForLeakedPins();
                return crow::response(400);
            }
            std::string query = x["query"].s();
            
            // Execute query via engine
            std::string result = executor_->ExecuteQuery(query);
            CheckForLeakedPins();
            
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
    void CheckForLeakedPins() const {
#ifndef NDEBUG
        bpm_->AssertNoPinsHeld();
#endif
    }

    crow::response ServeStaticFile(const std::string& file_name, const std::string& content_type) const {
        const std::string path = dashboard_path_ + "/" + file_name;
        std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"),
                                                                &std::fclose);
        if (!file) {
            return crow::response(404, file_name + " not found");
        }

        std::string contents;
        std::array<char, 4096> chunk{};
        size_t bytes_read = 0;
        while ((bytes_read = std::fread(chunk.data(), 1, chunk.size(), file.get())) > 0) {
            contents.append(chunk.data(), bytes_read);
        }
        if (std::ferror(file.get()) != 0) {
            return crow::response(500, "Failed to read " + file_name);
        }
        auto res = crow::response(std::move(contents));
        res.add_header("Content-Type", content_type);
        return res;
    }

    std::shared_ptr<BufferPoolManager> bpm_;
    std::shared_ptr<IDiskManager> disk_manager_;
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
