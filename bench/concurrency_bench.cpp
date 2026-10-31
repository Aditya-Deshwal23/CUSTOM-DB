#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "executor.h"
#include "wal.h"

namespace relational_db {
namespace {

using Clock = std::chrono::steady_clock;

std::string DatabasePath() {
    return (std::filesystem::temp_directory_path() /
            ("custom-db-concurrency-bench-" +
             std::to_string(Clock::now().time_since_epoch().count())))
        .string();
}

struct DatabaseCleanup {
    explicit DatabaseCleanup(std::string database_path) : path(std::move(database_path)) {}

    ~DatabaseCleanup() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path + "-wal", error);
        std::filesystem::remove(path + "-wal.tmp", error);
        std::filesystem::remove(path + ".ack", error);
    }

    std::string path;
};

class BenchDatabase {
   public:
    explicit BenchDatabase(const std::string& path)
        : disk_(std::make_shared<DiskManager>(path)),
          wal_(path, DurabilityMode::OFF),
          buffer_pool_(std::make_shared<BufferPoolManager>(256, disk_)) {
        wal_.Recover(*disk_);
        executor_ = std::make_unique<Executor>(buffer_pool_, wal_);
        executor_->ExecuteQuery("CREATE TABLE bench_rows (id INT PRIMARY KEY, value INT NOT NULL)");
        std::string insert = "INSERT INTO bench_rows VALUES ";
        for (int32_t id = 0; id < 128; ++id) {
            if (id != 0) {
                insert += ',';
            }
            insert += '(' + std::to_string(id) + ", 0)";
        }
        executor_->ExecuteQuery(insert);
    }

    Executor& GetExecutor() {
        return *executor_;
    }

   private:
    std::shared_ptr<DiskManager> disk_;
    WriteAheadLog wal_;
    std::shared_ptr<BufferPoolManager> buffer_pool_;
    std::unique_ptr<Executor> executor_;
};

struct WorkloadResult {
    double operations_per_second{0.0};
    uint64_t p50_ns{0};
    uint64_t p99_ns{0};
};

WorkloadResult RunWorkload(Executor& executor, size_t thread_count, uint32_t operations_per_thread,
                           bool mixed, uint64_t seed) {
    std::atomic<size_t> ready{0};
    std::atomic<size_t> finished{0};
    std::atomic<bool> start{false};
    std::vector<std::vector<uint64_t>> samples(thread_count);
    std::vector<std::string> errors(thread_count);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (size_t worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([&, worker]() {
            std::mt19937_64 random(seed + worker);
            std::vector<uint64_t>& worker_samples = samples[worker];
            worker_samples.reserve(operations_per_thread);
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                for (uint32_t operation = 0; operation < operations_per_thread; ++operation) {
                    const int32_t id = static_cast<int32_t>(random() % 128U);
                    const bool write = mixed && random() % 20U == 0U;
                    const std::string query =
                        write
                            ? "UPDATE bench_rows SET value = value WHERE id = " + std::to_string(id)
                            : "SELECT value FROM bench_rows WHERE id = " + std::to_string(id);
                    const Clock::time_point operation_start = Clock::now();
                    const std::string result = executor.ExecuteQuery(query);
                    worker_samples.push_back(
                        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  Clock::now() - operation_start)
                                                  .count()));
                    if (result.empty()) {
                        throw std::runtime_error("statement returned an empty result");
                    }
                }
            } catch (const std::exception& error) {
                errors[worker] = error.what();
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    const Clock::time_point start_time = Clock::now();
    start.store(true, std::memory_order_release);
    while (finished.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    const Clock::time_point end_time = Clock::now();
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const std::string& error : errors) {
        if (!error.empty()) {
            throw std::runtime_error("concurrency workload failed: " + error);
        }
    }

    std::vector<uint64_t> latencies;
    for (const std::vector<uint64_t>& worker_samples : samples) {
        latencies.insert(latencies.end(), worker_samples.begin(), worker_samples.end());
    }
    if (latencies.empty()) {
        throw std::runtime_error("concurrency workload produced no latency samples");
    }
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&latencies](size_t numerator, size_t denominator) {
        const size_t index = (latencies.size() * numerator + denominator - 1U) / denominator - 1U;
        return latencies[index];
    };
    const double seconds = std::chrono::duration<double>(end_time - start_time).count();
    return WorkloadResult{static_cast<double>(latencies.size()) / seconds, percentile(50, 100),
                          percentile(99, 100)};
}

void PrintResult(const std::string& workload, size_t threads, const WorkloadResult& result) {
    std::cout << workload << " threads=" << threads
              << " operations_per_second=" << result.operations_per_second
              << " p50_ns=" << result.p50_ns << " p99_ns=" << result.p99_ns << '\n';
}

}  // namespace
}  // namespace relational_db

int main() {
    using namespace relational_db;
    try {
        constexpr uint64_t seed = 0xC0FFEEU;
        std::cout << "Concurrency benchmark seed=" << seed
                  << " (commits sync WAL in every durability mode)\n";
        const std::string path = DatabasePath();
        DatabaseCleanup cleanup(path);
        BenchDatabase database(path);
        for (const size_t threads : std::array<size_t, 4>{1, 2, 4, 8}) {
            PrintResult("point_select", threads,
                        RunWorkload(database.GetExecutor(), threads, 4000, false, seed));
        }
        PrintResult("read_write_95_5", 8,
                    RunWorkload(database.GetExecutor(), 8, 2000, true, seed + 1U));
    } catch (const std::exception& error) {
        std::cerr << "Concurrency benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
