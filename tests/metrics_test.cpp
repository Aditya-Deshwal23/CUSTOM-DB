#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "executor.h"
#include "metrics/metrics.h"
#include "wal.h"

#include <gtest/gtest.h>

namespace relational_db {
namespace {

class TemporaryMetricsDatabase {
   public:
    TemporaryMetricsDatabase()
        : path_(std::filesystem::temp_directory_path() /
                ("custom-db-metrics-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {}

    ~TemporaryMetricsDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-wal.tmp", error);
        std::filesystem::remove(path_.string() + ".ack", error);
    }

    std::string Path() const {
        return path_.string();
    }

   private:
    std::filesystem::path path_;
};

}  // namespace

TEST(MetricsTest, HistogramUsesFixedLog2MicrosecondBuckets) {
    Histogram histogram;
    histogram.Observe(0);
    histogram.Observe(1);
    histogram.Observe(2);
    histogram.Observe(3);
    histogram.Observe(8);

    const HistogramSnapshot snapshot = histogram.Snapshot();
    EXPECT_EQ(snapshot.count, 5U);
    EXPECT_EQ(snapshot.sum, 14U);
    EXPECT_EQ(snapshot.buckets[0], 2U);
    EXPECT_EQ(snapshot.buckets[1], 1U);
    EXPECT_EQ(snapshot.buckets[2], 1U);
    EXPECT_EQ(snapshot.buckets[3], 1U);
    EXPECT_EQ(snapshot.buckets[4], 0U);
    EXPECT_EQ(snapshot.Quantile(0.50), 2U);
    EXPECT_EQ(snapshot.Quantile(0.95), 8U);
    EXPECT_EQ(snapshot.Quantile(0.99), 8U);
}

TEST(MetricsTest, ConcurrentCounterUpdatesAreExact) {
    std::cout << "Metrics counter stress seed: 20261031\n";
    Counter counter;
    constexpr uint64_t kIncrementsPerThread = 100000;
    std::array<std::thread, 8> threads;
    for (std::thread& thread : threads) {
        thread = std::thread([&counter]() {
            for (uint64_t index = 0; index < kIncrementsPerThread; ++index) {
                counter.Increment();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(counter.Value(), 8U * kIncrementsPerThread);
}

TEST(MetricsTest, LabelsAreStableAndAllocationFreeToSelect) {
    LabeledCounter<2> operations({"read", "write"});
    operations.ForLabel(0).Increment();
    operations.ForLabel(1).Add(2);
    EXPECT_EQ(operations.Label(0), "read");
    EXPECT_EQ(operations.ForLabel(0).Value(), 1U);
    EXPECT_EQ(operations.ForLabel(1).Value(), 2U);
}

TEST(MetricsTest, RecordingCanBeDisabledForAControlBenchmark) {
    Counter counter;
    SetMetricUpdatesEnabled(false);
    counter.Increment();
    SetMetricUpdatesEnabled(true);
    EXPECT_EQ(counter.Value(), 0U);
}

TEST(MetricsTest, BufferPoolSeparatesColdMissesFromWarmHits) {
    TemporaryMetricsDatabase database;
    auto disk = std::make_shared<DiskManager>(database.Path());
    disk->Extend(10);
    BufferPoolManager buffer_pool(32, disk);
    const BufferPoolStats before = buffer_pool.SnapshotStats();
    for (page_id_t page_id = 1; page_id <= 10; ++page_id) {
        ReadPageGuard page = buffer_pool.FetchRead(page_id);
    }
    const BufferPoolStats cold = buffer_pool.SnapshotStats();
    EXPECT_EQ(cold.misses - before.misses, 10U);
    EXPECT_EQ(cold.hits, before.hits);

    for (page_id_t page_id = 1; page_id <= 10; ++page_id) {
        ReadPageGuard page = buffer_pool.FetchRead(page_id);
    }
    const BufferPoolStats warm = buffer_pool.SnapshotStats();
    EXPECT_EQ(warm.misses, cold.misses);
    EXPECT_EQ(warm.hits - cold.hits, 10U);
}

TEST(MetricsTest, SqlEventsUpdateExactStatementAndRowCounters) {
    TemporaryMetricsDatabase database;
    auto disk = std::make_shared<DiskManager>(database.Path());
    WriteAheadLog wal(database.Path());
    ASSERT_TRUE(wal.Recover(*disk).ran);
    auto buffer_pool = std::make_shared<BufferPoolManager>(64, disk);
    Executor executor(buffer_pool, wal);

    MetricsRegistry& metrics = MetricsRegistry::Instance();
    std::array<uint64_t, 5> statements_before{};
    for (size_t index = 0; index < statements_before.size(); ++index) {
        statements_before[index] = metrics.statements[index].Value();
    }
    const uint64_t rows_read_before = metrics.rows_read.Value();
    const uint64_t rows_written_before = metrics.rows_written.Value();
    const uint64_t errors_before = metrics.ErrorCount();

    EXPECT_EQ(executor.ExecuteQuery(
                  "CREATE TABLE metrics_rows (id INT PRIMARY KEY, value INT)"),
              "Table created");
    EXPECT_EQ(executor.ExecuteQuery("INSERT INTO metrics_rows VALUES (1, 10), (2, 20)"),
              "2 row(s) affected");
    EXPECT_EQ(executor.ExecuteQuery("SELECT value FROM metrics_rows WHERE id = 1"), "value\n10");
    EXPECT_EQ(executor.ExecuteQuery("UPDATE metrics_rows SET value = 11 WHERE id = 1"),
              "1 row(s) affected");
    EXPECT_EQ(executor.ExecuteQuery("DELETE FROM metrics_rows WHERE id = 2"),
              "1 row(s) affected");
    EXPECT_EQ(executor.ExecuteQuery("SELECT 999999"), "Key 999999 not found.");

    EXPECT_EQ(metrics.statements[static_cast<size_t>(StatementMetric::DDL)].Value() -
                  statements_before[static_cast<size_t>(StatementMetric::DDL)],
              1U);
    EXPECT_EQ(metrics.statements[static_cast<size_t>(StatementMetric::INSERT)].Value() -
                  statements_before[static_cast<size_t>(StatementMetric::INSERT)],
              1U);
    EXPECT_EQ(metrics.statements[static_cast<size_t>(StatementMetric::SELECT)].Value() -
                  statements_before[static_cast<size_t>(StatementMetric::SELECT)],
              2U);
    EXPECT_EQ(metrics.statements[static_cast<size_t>(StatementMetric::UPDATE)].Value() -
                  statements_before[static_cast<size_t>(StatementMetric::UPDATE)],
              1U);
    EXPECT_EQ(metrics.statements[static_cast<size_t>(StatementMetric::DELETE)].Value() -
                  statements_before[static_cast<size_t>(StatementMetric::DELETE)],
              1U);
    EXPECT_EQ(metrics.rows_read.Value() - rows_read_before, 3U);
    EXPECT_EQ(metrics.rows_written.Value() - rows_written_before, 4U);
    EXPECT_EQ(metrics.ErrorCount(), errors_before);
}

}  // namespace relational_db
