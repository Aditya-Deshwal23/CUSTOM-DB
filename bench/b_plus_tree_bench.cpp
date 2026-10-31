#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "b_plus_tree.h"
#include "disk_manager.h"

namespace {

using relational_db::BPlusTree;
using relational_db::BufferPoolManager;
using relational_db::DiskManager;
using relational_db::IndexKey16;
using relational_db::IndexKey16Less;
using relational_db::IndexKeyKind;

using Tree = BPlusTree<IndexKey16, uint64_t, IndexKey16Less>;

struct Dataset {
    std::filesystem::path path;
    std::shared_ptr<DiskManager> disk;
    std::shared_ptr<BufferPoolManager> pool;
    std::unique_ptr<Tree> tree;
    relational_db::page_id_t meta_page_id{relational_db::INVALID_PAGE_ID};
    int32_t key_count{0};

    ~Dataset() {
        tree.reset();
        pool.reset();
        disk.reset();
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + ".wal", error);
    }
};

std::unique_ptr<Dataset> BuildDataset(int32_t key_count) {
    auto dataset = std::make_unique<Dataset>();
    dataset->path =
        std::filesystem::temp_directory_path() /
        ("custom-db-btree-benchmark-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    dataset->disk = std::make_shared<DiskManager>(dataset->path.string());
    dataset->pool = std::make_shared<BufferPoolManager>(256, dataset->disk);
    dataset->tree =
        std::make_unique<Tree>(dataset->pool, IndexKey16Less(), IndexKeyKind::INT32);
    dataset->meta_page_id = dataset->tree->MetaPageId();
    dataset->key_count = key_count;
    for (int32_t key = 0; key < key_count; ++key) {
        dataset->tree->Insert(relational_db::EncodeIndexKey(key),
                              static_cast<uint64_t>(key));
    }
    dataset->pool->FlushAll();
    return dataset;
}

int32_t ProbeKey(int64_t iteration, int32_t key_count) {
    const uint64_t multiplier = 11400714819323198485ULL;
    return static_cast<int32_t>(
        ((static_cast<uint64_t>(iteration) * multiplier) % static_cast<uint64_t>(key_count)));
}

void BM_BPlusTreePointWarm(benchmark::State& state) {
    std::unique_ptr<Dataset> dataset;
    int64_t operation_index = 0;
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            dataset = BuildDataset(static_cast<int32_t>(state.range(0)));
            state.ResumeTiming();
        }
        const int32_t key = ProbeKey(operation_index++, dataset->key_count);
        const auto value = dataset->tree->GetValue(relational_db::EncodeIndexKey(key));
        uint64_t observed = value.value_or(0);
        benchmark::DoNotOptimize(observed);
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BPlusTreePointCold(benchmark::State& state) {
    std::unique_ptr<Dataset> dataset;
    int64_t operation_index = 0;
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            dataset = BuildDataset(static_cast<int32_t>(state.range(0)));
            state.ResumeTiming();
        }
        auto cold_pool = std::make_shared<BufferPoolManager>(16, dataset->disk);
        Tree cold_tree(cold_pool, IndexKey16Less(), dataset->meta_page_id, IndexKeyKind::INT32);
        const int32_t key = ProbeKey(operation_index++, dataset->key_count);
        const auto value = cold_tree.GetValue(relational_db::EncodeIndexKey(key));
        uint64_t observed = value.value_or(0);
        benchmark::DoNotOptimize(observed);
        cold_pool.reset();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BPlusTreeInsertWarm(benchmark::State& state) {
    std::unique_ptr<Dataset> dataset;
    int64_t operation_index = 0;
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            dataset = BuildDataset(static_cast<int32_t>(state.range(0)));
            state.ResumeTiming();
        }
        const int64_t key = static_cast<int64_t>(dataset->key_count) + operation_index++;
        benchmark::DoNotOptimize(dataset->tree->Insert(
            relational_db::EncodeIndexKey(static_cast<int32_t>(key)),
            static_cast<uint64_t>(key)));
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BPlusTreeInsertCold(benchmark::State& state) {
    std::unique_ptr<Dataset> dataset;
    int64_t operation_index = 0;
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            dataset = BuildDataset(static_cast<int32_t>(state.range(0)));
            state.ResumeTiming();
        }
        auto cold_pool = std::make_shared<BufferPoolManager>(16, dataset->disk);
        Tree cold_tree(cold_pool, IndexKey16Less(), dataset->meta_page_id, IndexKeyKind::INT32);
        const int64_t key = static_cast<int64_t>(dataset->key_count) + operation_index++;
        benchmark::DoNotOptimize(cold_tree.Insert(
            relational_db::EncodeIndexKey(static_cast<int32_t>(key)),
            static_cast<uint64_t>(key)));
        cold_pool.reset();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BPlusTreeDeleteWarm(benchmark::State& state) {
    std::unique_ptr<Dataset> dataset;
    int32_t next_key = 0;
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            dataset = BuildDataset(static_cast<int32_t>(state.range(0)));
            state.ResumeTiming();
        }
        if (next_key == dataset->key_count) {
            state.PauseTiming();
            for (int32_t key = 0; key < dataset->key_count; ++key) {
                dataset->tree->Insert(relational_db::EncodeIndexKey(key),
                                      static_cast<uint64_t>(key));
            }
            next_key = 0;
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(dataset->tree->Delete(relational_db::EncodeIndexKey(next_key++)));
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BPlusTreeRangeWarm(benchmark::State& state) {
    std::unique_ptr<Dataset> dataset;
    int64_t operation_index = 0;
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            dataset = BuildDataset(static_cast<int32_t>(state.range(0)));
            state.ResumeTiming();
        }
        const int32_t low = ProbeKey(operation_index++, dataset->key_count);
        const int32_t high = std::min<int32_t>(dataset->key_count, low + 128);
        const auto values = dataset->tree->RangeScan(relational_db::EncodeIndexKey(low),
                                                     relational_db::EncodeIndexKey(high));
        auto* first_value = values.data();
        size_t result_size = values.size();
        benchmark::DoNotOptimize(first_value);
        benchmark::DoNotOptimize(result_size);
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BPlusTreeRangeCold(benchmark::State& state) {
    std::unique_ptr<Dataset> dataset;
    int64_t operation_index = 0;
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            dataset = BuildDataset(static_cast<int32_t>(state.range(0)));
            state.ResumeTiming();
        }
        auto cold_pool = std::make_shared<BufferPoolManager>(16, dataset->disk);
        Tree cold_tree(cold_pool, IndexKey16Less(), dataset->meta_page_id, IndexKeyKind::INT32);
        const int32_t low = ProbeKey(operation_index++, dataset->key_count);
        const int32_t high = std::min<int32_t>(dataset->key_count, low + 128);
        const auto values = cold_tree.RangeScan(relational_db::EncodeIndexKey(low),
                                                relational_db::EncodeIndexKey(high));
        auto* first_value = values.data();
        size_t result_size = values.size();
        benchmark::DoNotOptimize(first_value);
        benchmark::DoNotOptimize(result_size);
        cold_pool.reset();
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_BPlusTreePointWarm)->Arg(10000)->Arg(100000)->Arg(1000000);
BENCHMARK(BM_BPlusTreePointCold)->Arg(10000)->Arg(100000)->Arg(1000000);
BENCHMARK(BM_BPlusTreeInsertWarm)->Arg(10000)->Arg(100000)->Arg(1000000);
BENCHMARK(BM_BPlusTreeInsertCold)->Arg(10000)->Arg(100000)->Arg(1000000);
BENCHMARK(BM_BPlusTreeDeleteWarm)->Arg(10000)->Arg(100000)->Arg(1000000);
BENCHMARK(BM_BPlusTreeRangeWarm)->Arg(10000)->Arg(100000)->Arg(1000000);
BENCHMARK(BM_BPlusTreeRangeCold)->Arg(10000)->Arg(100000)->Arg(1000000);

}  // namespace

BENCHMARK_MAIN();
