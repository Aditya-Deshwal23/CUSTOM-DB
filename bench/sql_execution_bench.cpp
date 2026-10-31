#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "executor.h"
#include "wal.h"

namespace relational_db {
namespace {

using Clock = std::chrono::steady_clock;

std::string DatabasePath(uint64_t row_count) {
    const uint64_t stamp = static_cast<uint64_t>(Clock::now().time_since_epoch().count());
    return (std::filesystem::temp_directory_path() /
            ("custom-db-sql-bench-" + std::to_string(row_count) + "-" + std::to_string(stamp)))
        .string();
}

void RemoveDatabase(const std::string& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path + "-wal", error);
    std::filesystem::remove(path + "-wal.tmp", error);
    std::filesystem::remove(path + ".ack", error);
}

struct DatabaseCleanup {
    std::string path;

    ~DatabaseCleanup() {
        RemoveDatabase(path);
    }
};

template <typename Operation>
double MeasureNanoseconds(uint64_t iterations, Operation operation) {
    const Clock::time_point start = Clock::now();
    for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
        operation(iteration);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    return static_cast<double>(elapsed.count()) / static_cast<double>(iterations);
}

void Populate(Executor& executor, uint64_t row_count) {
    constexpr uint64_t kRowsPerInsert = 64;
    for (uint64_t first = 0; first < row_count; first += kRowsPerInsert) {
        std::ostringstream query;
        query << "INSERT INTO benchmark_rows VALUES ";
        const uint64_t end = std::min(row_count, first + kRowsPerInsert);
        for (uint64_t id = first; id < end; ++id) {
            if (id != first) {
                query << ',';
            }
            query << '(' << id << ',' << (id % 1000U) << ')';
        }
        executor.ExecuteQuery(query.str());
    }
}

void RunScale(uint64_t row_count) {
    const std::string path = DatabasePath(row_count);
    DatabaseCleanup cleanup{path};
    auto disk = std::make_shared<DiskManager>(path);
    WriteAheadLog wal(path, DurabilityMode::OFF);
    wal.Recover(*disk);
    auto buffer_pool = std::make_shared<BufferPoolManager>(4096, disk);
    Executor executor(buffer_pool, wal);

    executor.ExecuteQuery("CREATE TABLE benchmark_rows (id BIGINT PRIMARY KEY, payload BIGINT)");
    Populate(executor, row_count);

    const uint64_t iterations = 100;
    const double point_ns = MeasureNanoseconds(iterations, [&](uint64_t iteration) {
        const uint64_t id = (iteration * 7919U) % row_count;
        executor.ExecuteQuery("SELECT payload FROM benchmark_rows WHERE id = " +
                              std::to_string(id));
    });
    const double range_ns = MeasureNanoseconds(iterations, [&](uint64_t iteration) {
        const uint64_t start = (iteration * 101U) % (row_count - 99U);
        executor.ExecuteQuery("SELECT id FROM benchmark_rows WHERE id BETWEEN " +
                              std::to_string(start) + " AND " + std::to_string(start + 99U) +
                              " ORDER BY id");
    });
    const double insert_ns = MeasureNanoseconds(iterations, [&](uint64_t iteration) {
        const uint64_t id = row_count + iteration;
        executor.ExecuteQuery("INSERT INTO benchmark_rows VALUES (" + std::to_string(id) + ", 1)");
    });
    const double update_ns = MeasureNanoseconds(iterations, [&](uint64_t iteration) {
        const uint64_t id = row_count + iteration;
        executor.ExecuteQuery("UPDATE benchmark_rows SET payload = payload + 1 WHERE id = " +
                              std::to_string(id));
    });
    const double aggregate_ns = MeasureNanoseconds(5, [&](uint64_t) {
        executor.ExecuteQuery(
            "SELECT COUNT(*), SUM(payload), MIN(payload), MAX(payload), "
            "AVG(payload) FROM benchmark_rows");
    });

    std::cout << "rows=" << row_count << " durability=off"
              << " point_select_ns=" << point_ns << " range_100_ns=" << range_ns
              << " insert_ns=" << insert_ns << " update_ns=" << update_ns
              << " full_scan_aggregate_ns=" << aggregate_ns << '\n';
}

}  // namespace
}  // namespace relational_db

int main(int argc, char** argv) {
    using namespace relational_db;
    std::vector<uint64_t> scales;
    if (argc == 1) {
        scales = {10000, 100000, 1000000};
    } else {
        for (int argument = 1; argument < argc; ++argument) {
            scales.push_back(std::stoull(argv[argument]));
        }
    }
    std::cout << "SQL execution benchmark uses WAL durability=off; results are not "
                 "durability measurements.\n";
    for (uint64_t scale : scales) {
        if (scale < 100U) {
            std::cerr << "row counts must be at least 100\n";
            return 2;
        }
        RunScale(scale);
    }
    return 0;
}
