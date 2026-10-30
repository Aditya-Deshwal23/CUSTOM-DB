#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <random>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <csignal>
#include "buffer_pool_manager.h"
#include "slotted_page.h"
#include "b_plus_tree.h"
#include "executor.h"
#include "api_server.h"

using namespace relational_db;

namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

void HandleShutdownSignal(int) {
    g_shutdown_requested = 1;
}

int ReadPortFromEnv() {
    const char* port_env = std::getenv("PORT");
    if (port_env == nullptr || std::string(port_env).empty()) {
        return 8080;
    }

    try {
        int port = std::stoi(port_env);
        if (port > 0 && port <= 65535) {
            return port;
        }
    } catch (const std::exception&) {
    }

    std::cerr << "Invalid PORT='" << port_env << "', defaulting to 8080" << std::endl;
    return 8080;
}

std::string ReadDbPathFromEnv() {
    const char* db_path_env = std::getenv("DB_PATH");
    if (db_path_env == nullptr || std::string(db_path_env).empty()) {
        return "./test.db";
    }
    return db_path_env;
}

std::string ResolveDashboardPath() {
    if (std::filesystem::exists("dashboard/index.html")) {
        return "dashboard";
    }
    if (std::filesystem::exists("../dashboard/index.html")) {
        return "../dashboard";
    }
    return "dashboard";
}

} // namespace

int main() {
    std::signal(SIGINT, HandleShutdownSignal);
    std::signal(SIGTERM, HandleShutdownSignal);

    std::cout << "Custom Relational Storage Engine Initialized (Native UCRT64 Mode)" << std::endl;

    const int port = ReadPortFromEnv();
    const std::string db_path = ReadDbPathFromEnv();
    const std::string dashboard_path = ResolveDashboardPath();

    std::cout << "Selected PORT: " << port << std::endl;
    std::cout << "Selected DB_PATH: " << db_path << std::endl;
    std::cout << "Dashboard static path: " << dashboard_path << std::endl;

    std::shared_ptr<DiskManager> disk_manager;
    try {
        disk_manager = std::make_shared<DiskManager>(db_path);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to initialize database at '" << db_path << "': " << ex.what()
                  << std::endl;
        return 1;
    }
    auto bpm = std::make_shared<BufferPoolManager>(disk_manager);
    auto executor = std::make_shared<Executor>(bpm);
    auto api_server = std::make_shared<ApiServer>(bpm, disk_manager, executor, dashboard_path);

    BPlusTree<int, int, std::less<int>> tree(bpm, std::less<int>());
    for (int i = 1; i <= 100; ++i) {
        tree.Insert(i, i * 100);
    }

    std::atomic<bool> running{true};

    std::thread signal_thread([api_server, &running]() {
        while (running.load() && g_shutdown_requested == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (g_shutdown_requested != 0) {
            std::cout << "Shutdown signal received, stopping server..." << std::endl;
            running.store(false);
            api_server->Stop();
        }
    });

    std::thread workload_thread([&tree, bpm, api_server, &running]() {
        std::mt19937 rng(1337);
        std::uniform_int_distribution<int> dist_op(0, 10);
        std::uniform_int_distribution<int> dist_key(1, 200);

        int queries_this_sec = 0;
        int total_latency = 0;
        int errors = 0;
        uint64_t last_hits = 0;
        uint64_t last_misses = 0;

        auto last_tick = std::chrono::steady_clock::now();

        while (running.load()) {
            auto start_q = std::chrono::steady_clock::now();
            int op = dist_op(rng);
            int key = dist_key(rng);
            
            if (op < 7) {
                std::vector<int> res;
                if (!tree.GetValue(key, &res)) errors++;
            } else {
                if (!tree.Insert(key, key * 10)) errors++;
            }

            auto end_q = std::chrono::steady_clock::now();
            total_latency += std::chrono::duration_cast<std::chrono::milliseconds>(end_q - start_q).count();
            queries_this_sec++;

            std::this_thread::sleep_for(std::chrono::milliseconds(dist_op(rng) + 5));

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_tick).count() >= 1) {
                int avg_latency = queries_this_sec > 0 ? (total_latency / queries_this_sec) : 0;
                const uint64_t current_hits = bpm->GetHits();
                const uint64_t current_misses = bpm->GetMisses();
                
                const uint64_t delta_hits = current_hits - last_hits;
                const uint64_t delta_misses = current_misses - last_misses;
                const uint64_t delta_total = delta_hits + delta_misses;
                const int hit_rate =
                    delta_total == 0 ? 0 : static_cast<int>((delta_hits * 100U) / delta_total);

                api_server->SetCacheHitRate(hit_rate);
                api_server->SetMetrics(queries_this_sec, avg_latency, errors);

                last_hits = current_hits;
                last_misses = current_misses;
                queries_this_sec = 0;
                total_latency = 0;
                errors = 0;
                last_tick = now;
            }
        }
    });

    std::cout << "Starting Web Dashboard at http://0.0.0.0:" << port << std::endl;
    api_server->Run(port);

    running.store(false);

    if (signal_thread.joinable()) {
        signal_thread.join();
    }

    if (workload_thread.joinable()) {
        workload_thread.join();
    }

    std::cout << "Shutdown complete." << std::endl;
    return 0;
}
