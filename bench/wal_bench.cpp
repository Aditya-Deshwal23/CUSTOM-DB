#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "b_plus_tree.h"
#include "benchmark/benchmark.h"
#include "disk_manager.h"
#include "wal.h"

namespace {

using namespace relational_db;

std::string NewDatabasePath() {
    static std::atomic<uint64_t> next_id{0};
    return (std::filesystem::temp_directory_path() /
            ("custom-db-wal-bench-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(next_id.fetch_add(1, std::memory_order_relaxed))))
        .string();
}

class DatabaseFixture {
   public:
    explicit DatabaseFixture(DurabilityMode mode)
        : path_(NewDatabasePath()),
          disk_(std::make_shared<DiskManager>(path_)),
          wal_(std::make_unique<WriteAheadLog>(path_, mode, 20, 1ULL << 30U)),
          buffer_pool_(std::make_shared<BufferPoolManager>(64, disk_)) {
        wal_->Recover(*disk_);
        auto transaction = buffer_pool_->BeginTransaction(*wal_);
        tree_ = std::make_unique<BPlusTree<int, int64_t, std::less<int>>>(buffer_pool_,
                                                                          std::less<int>());
        transaction->Commit();
    }

    DatabaseFixture(const DatabaseFixture&) = delete;
    DatabaseFixture& operator=(const DatabaseFixture&) = delete;

    ~DatabaseFixture() {
        tree_.reset();
        buffer_pool_.reset();
        wal_.reset();
        disk_.reset();
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_ + "-wal", error);
        std::filesystem::remove(path_ + "-wal.tmp", error);
    }

    BufferPoolManager& BufferPool() {
        return *buffer_pool_;
    }
    BPlusTree<int, int64_t, std::less<int>>& Tree() {
        return *tree_;
    }
    WriteAheadLog& Wal() {
        return *wal_;
    }

   private:
    std::string path_;
    std::shared_ptr<DiskManager> disk_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::shared_ptr<BufferPoolManager> buffer_pool_;
    std::unique_ptr<BPlusTree<int, int64_t, std::less<int>>> tree_;
};

const char* ModeName(DurabilityMode mode) {
    switch (mode) {
        case DurabilityMode::FULL:
            return "full";
        case DurabilityMode::NORMAL:
            return "normal";
        case DurabilityMode::OFF:
            return "off";
    }
    return "unknown";
}

void WalCommitLatency(benchmark::State& state, DurabilityMode mode) {
    constexpr int kKeysPerStatement = 4;
    DatabaseFixture database(mode);
    int32_t key_base = 0;
    for (auto _ : state) {
        auto transaction = database.BufferPool().BeginTransaction(database.Wal());
        for (int index = 0; index < kKeysPerStatement; ++index) {
            benchmark::DoNotOptimize(
                database.Tree().Insert(key_base + index, static_cast<int64_t>(key_base + index)));
        }
        transaction->Commit();
        key_base += kKeysPerStatement;
    }
    state.SetLabel(ModeName(mode));
    state.SetItemsProcessed(state.iterations() * kKeysPerStatement);
}

void CheckpointPause(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        {
            DatabaseFixture database(DurabilityMode::FULL);
            for (int batch = 0; batch < 64; ++batch) {
                auto transaction = database.BufferPool().BeginTransaction(database.Wal());
                for (int index = 0; index < 4; ++index) {
                    const int key = batch * 4 + index;
                    benchmark::DoNotOptimize(database.Tree().Insert(key, key));
                }
                transaction->Commit();
            }
            state.ResumeTiming();
            database.Wal().Checkpoint(database.BufferPool());
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetLabel("full; 256 rows");
}

BENCHMARK_CAPTURE(WalCommitLatency, Full, DurabilityMode::FULL)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(WalCommitLatency, Normal, DurabilityMode::NORMAL)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(WalCommitLatency, Off, DurabilityMode::OFF)->Unit(benchmark::kMicrosecond);
BENCHMARK(CheckpointPause)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
