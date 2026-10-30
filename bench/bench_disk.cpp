#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "disk_manager.h"

namespace {

constexpr size_t kPageCount = 256;

struct BenchmarkDatabase {
    std::filesystem::path path;
    std::unique_ptr<relational_db::FileDiskManager> manager;

    BenchmarkDatabase()
        : path(std::filesystem::temp_directory_path() /
               ("custom-db-disk-bench-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        manager = std::make_unique<relational_db::FileDiskManager>(path.string());
        manager->Extend(kPageCount);
        std::array<char, relational_db::PAGE_DATA_SIZE> page{};
        for (size_t index = 0; index < page.size(); ++index) {
            page[index] = static_cast<char>((index * 17U + 3U) & 0xFFU);
        }
        for (size_t page_id = 1; page_id <= kPageCount; ++page_id) {
            manager->WritePage(static_cast<relational_db::page_id_t>(page_id), page.data());
        }
        manager->Sync();
    }

    ~BenchmarkDatabase() {
        manager.reset();
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

void ReadPages(benchmark::State& state, bool random_access) {
    BenchmarkDatabase database;
    std::array<char, relational_db::PAGE_DATA_SIZE> page{};
    std::minstd_rand random(20261030);
    std::uniform_int_distribution<size_t> page_distribution(1, kPageCount);
    size_t sequential_page = 1;
    for (auto _ : state) {
        const size_t page_id = random_access ? page_distribution(random) : sequential_page;
        database.manager->ReadPage(static_cast<relational_db::page_id_t>(page_id), page.data());
        benchmark::DoNotOptimize(page.data());
        sequential_page = sequential_page % kPageCount + 1;
    }
    state.SetBytesProcessed(state.iterations() * relational_db::PAGE_SIZE);
}

void WritePages(benchmark::State& state, bool random_access) {
    BenchmarkDatabase database;
    std::array<char, relational_db::PAGE_DATA_SIZE> page{};
    page.fill('d');
    std::minstd_rand random(20261030);
    std::uniform_int_distribution<size_t> page_distribution(1, kPageCount);
    size_t sequential_page = 1;
    for (auto _ : state) {
        const size_t page_id = random_access ? page_distribution(random) : sequential_page;
        database.manager->WritePage(static_cast<relational_db::page_id_t>(page_id), page.data());
        benchmark::ClobberMemory();
        sequential_page = sequential_page % kPageCount + 1;
    }
    state.SetBytesProcessed(state.iterations() * relational_db::PAGE_SIZE);
}

void BM_SequentialRead(benchmark::State& state) {
    ReadPages(state, false);
}
void BM_RandomRead(benchmark::State& state) {
    ReadPages(state, true);
}
void BM_SequentialWrite(benchmark::State& state) {
    WritePages(state, false);
}
void BM_RandomWrite(benchmark::State& state) {
    WritePages(state, true);
}

void BM_SyncLatency(benchmark::State& state) {
    BenchmarkDatabase database;
    std::array<char, relational_db::PAGE_DATA_SIZE> page{};
    page.fill('f');
    for (auto _ : state) {
        state.PauseTiming();
        database.manager->WritePage(1, page.data());
        state.ResumeTiming();
        database.manager->Sync();
    }
}

BENCHMARK(BM_SequentialRead)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_RandomRead)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_SequentialWrite)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_RandomWrite)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_SyncLatency)->Unit(benchmark::kMicrosecond);

}  // namespace

BENCHMARK_MAIN();
