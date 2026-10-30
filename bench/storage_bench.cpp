#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "disk_manager.h"
#include "storage/table_heap.h"

namespace {

constexpr size_t kRowCount = 1'000'000;
constexpr size_t kRowWidth = 32;

class TemporaryDatabase {
   public:
    TemporaryDatabase() {
        path_ = std::filesystem::temp_directory_path() /
                ("custom-db-storage-bench-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    std::string String() const {
        return path_.string();
    }

   private:
    std::filesystem::path path_;
};

struct Dataset {
    explicit Dataset(TemporaryDatabase& database)
        : disk(std::make_shared<relational_db::FileDiskManager>(database.String())),
          buffer_pool(256, disk),
          heap(relational_db::TableHeap::Create(buffer_pool)) {
        relational_db::Tuple row(kRowWidth);
        for (size_t index = 0; index < kRowCount; ++index) {
            const uint64_t id = index;
            for (size_t byte = 0; byte < sizeof(id); ++byte) {
                row[byte] = static_cast<std::byte>(id >> (byte * 8U));
            }
            heap.Insert(row);
        }
    }

    std::shared_ptr<relational_db::FileDiskManager> disk;
    relational_db::BufferPoolManager buffer_pool;
    relational_db::TableHeap heap;
};

void BM_TableHeapInsertMillionRows(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        TemporaryDatabase database;
        auto disk = std::make_shared<relational_db::FileDiskManager>(database.String());
        relational_db::BufferPoolManager buffer_pool(256, disk);
        relational_db::TableHeap heap = relational_db::TableHeap::Create(buffer_pool);
        relational_db::Tuple row(kRowWidth);
        state.ResumeTiming();
        for (size_t index = 0; index < kRowCount; ++index) {
            const uint64_t id = index;
            for (size_t byte = 0; byte < sizeof(id); ++byte) {
                row[byte] = static_cast<std::byte>(id >> (byte * 8U));
            }
            heap.Insert(row);
        }
        benchmark::DoNotOptimize(heap.HeaderPageId());
        state.PauseTiming();
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(kRowCount));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(kRowCount * kRowWidth));
}

void BM_TableHeapFullScanMillionRows(benchmark::State& state) {
    std::unique_ptr<TemporaryDatabase> database;
    std::unique_ptr<Dataset> dataset;
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            database = std::make_unique<TemporaryDatabase>();
            dataset = std::make_unique<Dataset>(*database);
            state.ResumeTiming();
        }
        size_t rows = 0;
        auto iterator = dataset->heap.Begin();
        relational_db::RID rid;
        relational_db::Tuple tuple;
        while (iterator.Next(rid, tuple)) {
            benchmark::DoNotOptimize(tuple.data());
            ++rows;
        }
        if (rows != kRowCount) {
            state.SkipWithError("TableHeap scan did not return one million rows");
            break;
        }
        benchmark::DoNotOptimize(rows);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(kRowCount));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(kRowCount * kRowWidth));
}

void BM_TableHeapPointGet(benchmark::State& state) {
    std::unique_ptr<TemporaryDatabase> database;
    std::unique_ptr<Dataset> dataset;
    std::vector<relational_db::RID> rids;
    rids.reserve(kRowCount);
    std::mt19937_64 random(0x5eedULL);
    std::uniform_int_distribution<uint64_t> row_index(0, kRowCount - 1);
    for (auto _ : state) {
        if (!dataset) {
            state.PauseTiming();
            database = std::make_unique<TemporaryDatabase>();
            dataset = std::make_unique<Dataset>(*database);
            auto iterator = dataset->heap.Begin();
            relational_db::RID rid;
            relational_db::Tuple tuple;
            while (iterator.Next(rid, tuple)) {
                rids.push_back(rid);
            }
            if (rids.size() != kRowCount) {
                state.SkipWithError("TableHeap setup scan did not return one million RIDs");
                break;
            }
            state.ResumeTiming();
        }
        if (state.skipped()) {
            break;
        }
        const auto tuple = dataset->heap.Get(rids[static_cast<size_t>(row_index(random))]);
        if (tuple) {
            benchmark::DoNotOptimize(tuple->data());
        } else {
            state.SkipWithError("TableHeap point get returned no row");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_TableHeapInsertMillionRows)->Iterations(1);
BENCHMARK(BM_TableHeapFullScanMillionRows)->Iterations(1);
BENCHMARK(BM_TableHeapPointGet);

}  // namespace

BENCHMARK_MAIN();
