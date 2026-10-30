#include <benchmark/benchmark.h>

#include "buffer_pool_manager.h"
#include "disk_manager.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex g_policy_mutex;
std::unique_ptr<class PoolFixture> g_shared_throughput_fixture;

void SetPolicy(const std::string& policy) {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    const char* current = std::getenv("BUFFER_POOL_POLICY");
    if (current != nullptr && policy == current) {
        return;
    }
#ifdef _WIN32
    _putenv_s("BUFFER_POOL_POLICY", policy.c_str());
#else
    setenv("BUFFER_POOL_POLICY", policy.c_str(), 1);
#endif
}

class PoolFixture {
   public:
    PoolFixture(const std::string& policy, size_t frames = 64, uint64_t disk_pages = 512) {
        SetPolicy(policy);
        path_ = (std::filesystem::temp_directory_path() /
                 ("custom-db-buffer-bench-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "-" + std::to_string(next_id_++)))
                    .string();
        disk_ = std::make_shared<relational_db::FileDiskManager>(path_);
        disk_->Extend(disk_pages);
        pool_ = std::make_unique<relational_db::BufferPoolManager>(frames, disk_);
    }

    ~PoolFixture() {
        pool_.reset();
        disk_.reset();
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    relational_db::BufferPoolManager& Pool() { return *pool_; }

   private:
    inline static uint64_t next_id_{0};
    std::string path_;
    std::shared_ptr<relational_db::FileDiskManager> disk_;
    std::unique_ptr<relational_db::BufferPoolManager> pool_;
};

void SetupThroughput(const benchmark::State&) {
    g_shared_throughput_fixture = std::make_unique<PoolFixture>("clock", 64, 1024);
}

void SetupLruKThroughput(const benchmark::State&) {
    g_shared_throughput_fixture = std::make_unique<PoolFixture>("lruk", 64, 1024);
}

void TeardownThroughput(const benchmark::State&) {
    g_shared_throughput_fixture.reset();
}

void BM_BufferPoolHit(benchmark::State& state, const std::string& policy) {
    PoolFixture fixture(policy);
    {
        auto guard = fixture.Pool().FetchRead(1);
    }
    for (auto _ : state) {
        auto guard = fixture.Pool().FetchRead(1);
        benchmark::DoNotOptimize(guard.GetFrameId());
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BufferPoolMiss(benchmark::State& state, const std::string& policy) {
    PoolFixture fixture(policy, 64, 1024);
    uint64_t page_id = 1;
    for (auto _ : state) {
        auto guard = fixture.Pool().FetchRead(static_cast<relational_db::page_id_t>(page_id));
        benchmark::DoNotOptimize(guard.GetFrameId());
        page_id = page_id % 1024U + 1U;
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BufferPoolThroughput(benchmark::State& state, const std::string&) {
    uint64_t operation = static_cast<uint64_t>(state.thread_index());
    for (auto _ : state) {
        const auto page_id = static_cast<relational_db::page_id_t>(1U + operation % 512U);
        auto guard = g_shared_throughput_fixture->Pool().FetchRead(page_id);
        benchmark::DoNotOptimize(guard.GetFrameId());
        operation += 31U;
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_ScanResistance(benchmark::State& state, const std::string& policy) {
    PoolFixture fixture(policy, 64, 512);
    auto& pool = fixture.Pool();
    for (relational_db::page_id_t page_id = 1; page_id <= 16; ++page_id) {
        auto guard = pool.FetchRead(page_id);
    }
    uint64_t hot_hits = 0;
    uint64_t hot_accesses = 0;
    for (auto _ : state) {
        const auto before = pool.SnapshotStats();
        for (relational_db::page_id_t hot = 1; hot <= 16; ++hot) {
            auto guard = pool.FetchRead(hot);
            benchmark::DoNotOptimize(guard.GetFrameId());
        }
        const auto after_hot = pool.SnapshotStats();
        hot_hits += after_hot.hits - before.hits;
        hot_accesses += 16;
        for (relational_db::page_id_t flood = 17; flood <= 272; ++flood) {
            auto guard = pool.FetchRead(flood);
            benchmark::DoNotOptimize(guard.GetFrameId());
        }
    }
    const double hot_hit_rate =
        hot_accesses == 0 ? 0.0
                          : 100.0 * static_cast<double>(hot_hits) /
                                static_cast<double>(hot_accesses);
    state.counters["hot_hit_rate_percent"] =
        benchmark::Counter(hot_hit_rate * static_cast<double>(state.iterations()),
                           benchmark::Counter::kAvgIterations);
    state.SetItemsProcessed(static_cast<int64_t>(hot_accesses));
}

BENCHMARK_CAPTURE(BM_BufferPoolHit, Clock, "clock");
BENCHMARK_CAPTURE(BM_BufferPoolHit, LruK, "lruk");
BENCHMARK_CAPTURE(BM_BufferPoolMiss, Clock, "clock");
BENCHMARK_CAPTURE(BM_BufferPoolMiss, LruK, "lruk");
BENCHMARK_CAPTURE(BM_BufferPoolThroughput, Clock, "clock")
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Setup(SetupThroughput)
    ->Teardown(TeardownThroughput);
BENCHMARK_CAPTURE(BM_BufferPoolThroughput, LruK, "lruk")
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Setup(SetupLruKThroughput)
    ->Teardown(TeardownThroughput);
BENCHMARK_CAPTURE(BM_ScanResistance, Clock, "clock");
BENCHMARK_CAPTURE(BM_ScanResistance, LruK, "lruk");

}  // namespace

BENCHMARK_MAIN();
