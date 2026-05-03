#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <random>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include "buffer_pool_manager.h"
#include "slotted_page.h"
#include "b_plus_tree.h"
#include "executor.h"
#include "api_server.h"

using namespace relational_db;

int main() {
    std::cout << "Custom Relational Storage Engine Initialized (Native UCRT64 Mode)" << std::endl;

    auto disk_manager = std::make_shared<DiskManager>("test.db");
    auto bpm = std::make_shared<BufferPoolManager>(50, disk_manager);
    auto executor = std::make_shared<Executor>(bpm);
    auto api_server = std::make_shared<ApiServer>(bpm, disk_manager, executor);

    BPlusTree<int, int, std::less<int>> tree(bpm, std::less<int>());
    for (int i = 1; i <= 100; ++i) {
        tree.Insert(i, i * 100);
    }

    // Start API Server in background using standard std::thread
    std::thread server_thread([api_server]() {
        std::cout << "Starting Web Dashboard at http://127.0.0.1:8080" << std::endl;
        api_server->Run(8080);
    });
    server_thread.detach();

    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist_op(0, 10);
    std::uniform_int_distribution<int> dist_key(1, 200);

    int queries_this_sec = 0;
    int total_latency = 0;
    int errors = 0;
    int last_hits = 0;
    int last_misses = 0;

    auto last_tick = std::chrono::steady_clock::now();

    while (true) {
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
            int current_hits = bpm->GetHits();
            int current_misses = bpm->GetMisses();
            
            int delta_total = (current_hits - last_hits) + (current_misses - last_misses);
            int hit_rate = delta_total == 0 ? 0 : ((current_hits - last_hits) * 100) / delta_total;

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

    return 0;
}
