#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    uint64_t records{100000};
    uint64_t operations{100000};
    uint64_t seed{1337};
    size_t threads{1};
    size_t frames{256};
    std::string workload{"A"};
    std::string distribution{"uniform"};
    std::string durability{"full"};
    std::string cache{"warm"};
};

struct Database {
    explicit Database(const Options& options)
        : path((std::filesystem::temp_directory_path() /
                ("custom-db-ycsb-" + std::to_string(Clock::now().time_since_epoch().count())))
                   .string()),
          disk(std::make_shared<relational_db::DiskManager>(path)),
          wal(std::make_unique<relational_db::WriteAheadLog>(path,
                                                             ParseDurability(options.durability))),
          pool(std::make_shared<relational_db::BufferPoolManager>(options.frames, disk)) {
        wal->Recover(*disk);
        executor = std::make_unique<relational_db::Executor>(pool, *wal);
        executor->ExecuteQuery("CREATE TABLE ycsb (id BIGINT PRIMARY KEY, value BIGINT NOT NULL)");
    }

    ~Database() {
        executor.reset();
        pool.reset();
        wal.reset();
        disk.reset();
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path + "-wal", error);
        std::filesystem::remove(path + "-wal.tmp", error);
        std::filesystem::remove(path + ".ack", error);
    }

    void Populate(uint64_t records) {
        constexpr uint64_t kBatchSize = 512;
        for (uint64_t first = 0; first < records; first += kBatchSize) {
            std::string sql = "INSERT INTO ycsb VALUES ";
            const uint64_t end = std::min(records, first + kBatchSize);
            for (uint64_t id = first; id < end; ++id) {
                if (id != first) {
                    sql += ',';
                }
                sql += '(' + std::to_string(id) + ", 0)";
            }
            executor->ExecuteQuery(sql);
        }
    }

    void ReopenPool(const Options& options) {
        executor.reset();
        pool.reset();
        pool = std::make_shared<relational_db::BufferPoolManager>(options.frames, disk);
        executor = std::make_unique<relational_db::Executor>(pool, *wal);
    }

    static relational_db::DurabilityMode ParseDurability(const std::string& value) {
        if (value == "full") {
            return relational_db::DurabilityMode::FULL;
        }
        if (value == "normal") {
            return relational_db::DurabilityMode::NORMAL;
        }
        throw std::invalid_argument("durability must be full or normal");
    }

    std::string path;
    std::shared_ptr<relational_db::DiskManager> disk;
    std::unique_ptr<relational_db::WriteAheadLog> wal;
    std::shared_ptr<relational_db::BufferPoolManager> pool;
    std::unique_ptr<relational_db::Executor> executor;
};

uint64_t ParseUnsigned(const std::string& value, const std::string& name) {
    size_t consumed = 0;
    const uint64_t parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0) {
        throw std::invalid_argument(name + " must be a positive integer");
    }
    return parsed;
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string flag(argv[index]);
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + flag);
        }
        const std::string value(argv[++index]);
        if (flag == "--records") {
            options.records = ParseUnsigned(value, flag);
        } else if (flag == "--operations") {
            options.operations = ParseUnsigned(value, flag);
        } else if (flag == "--seed") {
            options.seed = ParseUnsigned(value, flag);
        } else if (flag == "--threads") {
            options.threads = static_cast<size_t>(ParseUnsigned(value, flag));
        } else if (flag == "--frames") {
            options.frames = static_cast<size_t>(ParseUnsigned(value, flag));
        } else if (flag == "--workload") {
            options.workload = value;
        } else if (flag == "--distribution") {
            options.distribution = value;
        } else if (flag == "--durability") {
            options.durability = value;
        } else if (flag == "--cache") {
            options.cache = value;
        } else {
            throw std::invalid_argument("unknown option " + flag);
        }
    }
    if (options.records < 100000 || options.records > 1000000) {
        throw std::invalid_argument("--records must be between 100000 and 1000000");
    }
    if (options.workload != "A" && options.workload != "B" && options.workload != "C" &&
        options.workload != "E") {
        throw std::invalid_argument("--workload must be A, B, C, or E");
    }
    if (options.distribution != "uniform" && options.distribution != "zipfian") {
        throw std::invalid_argument("--distribution must be uniform or zipfian");
    }
    if (options.durability != "full" && options.durability != "normal") {
        throw std::invalid_argument("--durability must be full or normal");
    }
    if (options.cache != "warm" && options.cache != "cold") {
        throw std::invalid_argument("--cache must be warm or cold");
    }
    return options;
}

std::vector<double> BuildZipfCdf(uint64_t records) {
    constexpr double kExponent = 0.99;
    std::vector<double> cdf(static_cast<size_t>(records));
    double total = 0.0;
    for (uint64_t rank = 1; rank <= records; ++rank) {
        total += 1.0 / std::pow(static_cast<double>(rank), kExponent);
        cdf[static_cast<size_t>(rank - 1)] = total;
    }
    for (double& value : cdf) {
        value /= total;
    }
    return cdf;
}

uint64_t SelectKey(const Options& options, std::mt19937_64& random,
                   const std::vector<double>& zipf_cdf) {
    if (options.distribution == "uniform") {
        return std::uniform_int_distribution<uint64_t>(0, options.records - 1)(random);
    }
    const double sample = std::generate_canonical<double, 53>(random);
    return static_cast<uint64_t>(std::lower_bound(zipf_cdf.begin(), zipf_cdf.end(), sample) -
                                 zipf_cdf.begin());
}

uint64_t Percentile(const std::vector<uint64_t>& samples, uint64_t numerator) {
    const size_t index = (samples.size() * numerator + 99U) / 100U - 1U;
    return samples[index];
}

int Run(const Options& options) {
    Database database(options);
    database.Populate(options.records);
    database.pool->FlushAll();
    if (options.cache == "cold") {
        database.ReopenPool(options);
    }

    const auto zipf_cdf =
        options.distribution == "zipfian" ? BuildZipfCdf(options.records) : std::vector<double>{};
    std::atomic<size_t> ready{0};
    std::atomic<size_t> finished{0};
    std::atomic<bool> start{false};
    std::vector<std::vector<uint64_t>> samples(options.threads);
    std::vector<std::string> errors(options.threads);
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    const uint64_t operations_per_thread =
        (options.operations + options.threads - 1U) / options.threads;
    const auto before_stats = database.pool->SnapshotStats();

    try {
        for (size_t worker = 0; worker < options.threads; ++worker) {
            workers.emplace_back([&, worker]() {
                ready.fetch_add(1, std::memory_order_release);
                try {
                    std::mt19937_64 random(options.seed + worker);
                    std::vector<uint64_t>& latencies = samples[worker];
                    latencies.reserve(static_cast<size_t>(operations_per_thread));
                    while (!start.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    for (uint64_t operation = 0; operation < operations_per_thread; ++operation) {
                        const uint64_t key = SelectKey(options, random, zipf_cdf);
                        const uint64_t choice = random() % 100U;
                        const bool update = (options.workload == "A" && choice >= 50U) ||
                                            (options.workload == "B" && choice >= 95U);
                        std::string query;
                        if (options.workload == "E") {
                            const uint64_t end = std::min(options.records - 1U, key + 9U);
                            query = "SELECT id, value FROM ycsb WHERE id BETWEEN " +
                                    std::to_string(key) + " AND " + std::to_string(end) +
                                    " ORDER BY id";
                        } else if (update) {
                            query = "UPDATE ycsb SET value = value + 1 WHERE id = " +
                                    std::to_string(key);
                        } else {
                            query = "SELECT value FROM ycsb WHERE id = " + std::to_string(key);
                        }
                        const auto operation_start = Clock::now();
                        const std::string result = database.executor->ExecuteQuery(query);
                        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 Clock::now() - operation_start)
                                                 .count();
                        if (result.empty()) {
                            throw std::runtime_error("query returned an empty result");
                        }
                        latencies.push_back(static_cast<uint64_t>(elapsed));
                    }
                } catch (const std::exception& error) {
                    errors[worker] = error.what();
                }
                finished.fetch_add(1, std::memory_order_release);
            });
        }
    } catch (...) {
        start.store(true, std::memory_order_release);
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }
    while (ready.load(std::memory_order_acquire) != options.threads) {
        std::this_thread::yield();
    }
    const auto start_time = Clock::now();
    start.store(true, std::memory_order_release);
    while (finished.load(std::memory_order_acquire) != options.threads) {
        std::this_thread::yield();
    }
    const auto end_time = Clock::now();
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const std::string& error : errors) {
        if (!error.empty()) {
            throw std::runtime_error("YCSB worker failed: " + error);
        }
    }

    std::vector<uint64_t> all_samples;
    for (const auto& worker_samples : samples) {
        all_samples.insert(all_samples.end(), worker_samples.begin(), worker_samples.end());
    }
    if (all_samples.empty()) {
        throw std::runtime_error("YCSB workload produced no samples");
    }
    std::sort(all_samples.begin(), all_samples.end());
    const auto after_stats = database.pool->SnapshotStats();
    const uint64_t hit_delta = after_stats.hits - before_stats.hits;
    const uint64_t miss_delta = after_stats.misses - before_stats.misses;
    const double hit_rate =
        hit_delta + miss_delta == 0
            ? 0.0
            : 100.0 * static_cast<double>(hit_delta) / static_cast<double>(hit_delta + miss_delta);
    const double seconds = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << "workload,distribution,threads,records,operations,durability,cache,frames,seed,"
                 "throughput_ops_s,p50_ns,p95_ns,p99_ns,buffer_pool_hit_rate_percent\n";
    std::cout << options.workload << ',' << options.distribution << ',' << options.threads << ','
              << options.records << ',' << all_samples.size() << ',' << options.durability << ','
              << options.cache << ',' << options.frames << ',' << options.seed << ','
              << static_cast<double>(all_samples.size()) / seconds << ','
              << Percentile(all_samples, 50) << ',' << Percentile(all_samples, 95) << ','
              << Percentile(all_samples, 99) << ',' << hit_rate << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Run(ParseOptions(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "YCSB benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
