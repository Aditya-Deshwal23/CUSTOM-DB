#include <benchmark/benchmark.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "executor.h"
#include "metrics/metrics.h"
#include "wal.h"

namespace relational_db {
namespace {

class BenchmarkDatabase {
   public:
    BenchmarkDatabase()
        : path_(std::filesystem::temp_directory_path() /
                ("custom-db-metrics-bench-" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()))),
          disk_(std::make_shared<DiskManager>(path_.string())),
          wal_(std::make_unique<WriteAheadLog>(path_.string())) {
        wal_->Recover(*disk_);
        buffer_pool_ = std::make_shared<BufferPoolManager>(64, disk_);
        executor_ = std::make_unique<Executor>(buffer_pool_, *wal_);
        executor_->ExecuteQuery("CREATE TABLE metrics_bench (id INT PRIMARY KEY, value INT)");
        std::string insert = "INSERT INTO metrics_bench VALUES ";
        for (int32_t id = 0; id < 256; ++id) {
            if (id != 0) {
                insert += ", ";
            }
            insert += "(" + std::to_string(id) + ", " + std::to_string(id * 2) + ")";
        }
        executor_->ExecuteQuery(insert);
        (void)executor_->ExecuteQuery("SELECT value FROM metrics_bench WHERE id = 42");
    }

    ~BenchmarkDatabase() {
        executor_.reset();
        buffer_pool_.reset();
        wal_.reset();
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-wal.tmp", error);
        std::filesystem::remove(path_.string() + ".ack", error);
    }

    Executor& GetExecutor() {
        return *executor_;
    }

   private:
    std::filesystem::path path_;
    std::shared_ptr<DiskManager> disk_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::shared_ptr<BufferPoolManager> buffer_pool_;
    std::unique_ptr<Executor> executor_;
};

class MetricRecordingScope {
   public:
    explicit MetricRecordingScope(bool enabled) : previous_(MetricUpdatesEnabled()) {
        SetMetricUpdatesEnabled(enabled);
    }

    ~MetricRecordingScope() {
        SetMetricUpdatesEnabled(previous_);
    }

   private:
    bool previous_;
};

void PointSelectBenchmark(benchmark::State& state, bool enable_metrics) {
    BenchmarkDatabase database;
    MetricRecordingScope recording(enable_metrics);
    for (auto _ : state) {
        const std::string result =
            database.GetExecutor().ExecuteQuery("SELECT value FROM metrics_bench WHERE id = 42");
        benchmark::DoNotOptimize(result.size());
    }
}

void BM_PointSelectMetricsEnabled(benchmark::State& state) {
    PointSelectBenchmark(state, true);
}

void BM_PointSelectMetricsDisabled(benchmark::State& state) {
    PointSelectBenchmark(state, false);
}

BENCHMARK(BM_PointSelectMetricsEnabled);
BENCHMARK(BM_PointSelectMetricsDisabled);

}  // namespace
}  // namespace relational_db

BENCHMARK_MAIN();
