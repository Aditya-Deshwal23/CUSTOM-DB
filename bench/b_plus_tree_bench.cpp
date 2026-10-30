#include <benchmark/benchmark.h>

#include <chrono>
#include <filesystem>
#include <memory>

#include "b_plus_tree.h"
#include "disk_manager.h"

namespace {

void BM_BPlusTreeInsert(benchmark::State& state) {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("custom-db-benchmark-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    auto disk_manager = std::make_shared<relational_db::DiskManager>(path.string());
    auto buffer_pool = std::make_shared<relational_db::BufferPoolManager>(50, disk_manager);
    relational_db::BPlusTree<int, int, std::less<int>> tree(buffer_pool, std::less<int>());
    int key = 0;

    for (auto _ : state) {
        benchmark::DoNotOptimize(tree.Insert(key, key * 100));
        ++key;
    }

    buffer_pool.reset();
    disk_manager.reset();
    std::error_code error;
    std::filesystem::remove(path, error);
}

BENCHMARK(BM_BPlusTreeInsert);

}  // namespace

BENCHMARK_MAIN();
