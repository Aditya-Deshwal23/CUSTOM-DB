#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "b_plus_tree.h"
#include "disk_manager.h"
#include "executor.h"
#include "wal.h"

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

namespace {

using relational_db::BPlusTree;
using relational_db::BufferPoolManager;
using relational_db::DbError;
using relational_db::DiskManager;
using relational_db::Executor;
using relational_db::LogRecord;
using relational_db::LogRecordType;
using relational_db::WriteAheadLog;

class TemporaryPath {
   public:
    TemporaryPath() {
        static std::atomic<uint64_t> next_id{0};
        path_ = std::filesystem::temp_directory_path() /
                ("custom-db-regression-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(next_id.fetch_add(1)));
    }

    ~TemporaryPath() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + ".wal", error);
    }

    const std::string String() const {
        return path_.string();
    }

   private:
    std::filesystem::path path_;
};

std::shared_ptr<BufferPoolManager> MakeBufferPool(const std::string& path) {
    return std::make_shared<BufferPoolManager>(50, std::make_shared<DiskManager>(path));
}

LogRecord MakeRecord(uint8_t value) {
    LogRecord record{};
    record.type = LogRecordType::INSERT;
    record.page_id = 1;
    record.data = {value};
    return record;
}

}  // namespace

TEST(PersistenceTest, RestartKeepsData) {
    TemporaryPath path;
    {
        auto bpm = MakeBufferPool(path.String());
        BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
        for (int key = 1; key <= 10; ++key) {
            ASSERT_TRUE(tree.Insert(key, key * 100));
        }
    }

    auto bpm = MakeBufferPool(path.String());
    BPlusTree<int, int64_t, std::less<int>> reopened_tree(bpm, std::less<int>());
    const auto result = reopened_tree.GetValue(5);
    ASSERT_TRUE(result)
        << "Key 5 not found after reopening the same database file.";
    ASSERT_EQ(*result, 500);
}

TEST(PersistenceTest, ExecutorIndexAndRowsSurviveCleanRestart) {
    TemporaryPath path;
    {
        auto bpm = MakeBufferPool(path.String());
        Executor executor(bpm);
        EXPECT_EQ(executor.ExecuteQuery("INSERT 42 9001"), "Inserted (42, 9001)");
        EXPECT_EQ(executor.ExecuteQuery("SELECT 42"), "Result: 9001");
    }

    auto bpm = MakeBufferPool(path.String());
    Executor reopened_executor(bpm);
    EXPECT_EQ(reopened_executor.ExecuteQuery("SELECT 42"), "Result: 9001");
}

TEST(DurabilityTest, DISABLED_HardKillKeepsAckedWrites) {
#if !defined(__linux__)
    GTEST_SKIP() << "Hard-kill durability test requires Linux process semantics.";
#else
    TemporaryPath path;
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto bpm = MakeBufferPool(path.String());
        BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
        for (int key = 1; key <= 1000; ++key) {
            if (!tree.Insert(key, key * 100)) {
                _exit(17);
            }
        }
        _exit(0);
    }

    int child_status = 0;
    ASSERT_EQ(waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0);

    auto bpm = MakeBufferPool(path.String());
    BPlusTree<int, int64_t, std::less<int>> reopened_tree(bpm, std::less<int>());
    int missing = 0;
    for (int key = 1; key <= 1000; ++key) {
        if (!reopened_tree.GetValue(key)) {
            ++missing;
        }
    }
    ASSERT_EQ(missing, 0) << missing << " acknowledged inserts were lost after _exit.";
#endif
}

TEST(BPlusTreeTest, LookupsNeverFalseMissAfterManyInserts) {
    TemporaryPath path;
    auto bpm = MakeBufferPool(path.String());
    BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
    for (int key = 1; key <= 40000; ++key) {
        ASSERT_TRUE(tree.Insert(key, key * 100));
    }

    constexpr uint32_t seed = 1337;
    std::mt19937 random(seed);
    std::uniform_int_distribution<int> key_distribution(1, 40000);
    int misses = 0;
    int first_miss = 0;
    for (int lookup = 1; lookup <= 5000; ++lookup) {
        const int key = key_distribution(random);
        if (!tree.GetValue(key)) {
            ++misses;
            if (first_miss == 0) {
                first_miss = lookup;
            }
        }
    }

    ASSERT_EQ(misses, 0) << "misses=" << misses << ", first lookup=" << first_miss
                         << ", seed=" << seed;
}

TEST(BPlusTreeTest, RangeScanTerminatesAfterSplit) {
#if !defined(__linux__)
    TemporaryPath path;
    auto bpm = MakeBufferPool(path.String());
    BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
    for (int key = 1; key <= 1000; ++key) {
        ASSERT_TRUE(tree.Insert(key, key));
    }

    const auto values = tree.RangeScan(1, 100000);
    ASSERT_EQ(values.size(), 1000U);
    for (size_t index = 0; index < values.size(); ++index) {
        EXPECT_EQ(values[index].first, static_cast<int>(index + 1));
        EXPECT_EQ(values[index].second, static_cast<int64_t>(index + 1));
    }
    bpm->AssertNoPinsHeld();
#else
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        struct rlimit memory_limit{};
        memory_limit.rlim_cur = 1024ULL * 1024ULL * 1024ULL;
        memory_limit.rlim_max = memory_limit.rlim_cur;
        if (setrlimit(RLIMIT_AS, &memory_limit) != 0) {
            _exit(20);
        }
        alarm(5);

        TemporaryPath path;
        auto bpm = MakeBufferPool(path.String());
        BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
        for (int key = 1; key <= 1000; ++key) {
            if (!tree.Insert(key, key)) {
                _exit(21);
            }
        }

        const auto values = tree.RangeScan(1, 100000);
        alarm(0);
        if (values.size() != 1000) {
            _exit(22);
        }
        for (size_t index = 0; index < values.size(); ++index) {
            if (values[index].first != static_cast<int>(index + 1) ||
                values[index].second != static_cast<int64_t>(index + 1)) {
                _exit(23);
            }
        }
        _exit(0);
    }

    int child_status = 0;
    ASSERT_EQ(waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFEXITED(child_status))
        << "Range scan child terminated by signal " << WTERMSIG(child_status);
    ASSERT_EQ(WEXITSTATUS(child_status), 0);
#endif
}

TEST(WalTest, DISABLED_RecordAppendedAfterTornTailIsRecoverable) {
    TemporaryPath path;
    {
        WriteAheadLog wal(path.String());
        for (uint8_t value = 1; value <= 3; ++value) {
            LogRecord record = MakeRecord(value);
            wal.AppendLog(record);
        }
    }

    const auto original_size = std::filesystem::file_size(path.String());
    ASSERT_GT(original_size, 10U);
    std::filesystem::resize_file(path.String(), original_size - 10);
    {
        WriteAheadLog wal(path.String());
        ASSERT_EQ(wal.Recovery().size(), 2U);
        LogRecord record = MakeRecord(4);
        wal.AppendLog(record);
    }

    WriteAheadLog recovered(path.String());
    ASSERT_EQ(recovered.Recovery().size(), 3U);
}

TEST(ConcurrencyTest, ExecutorStatementGateTsanClean) {
    TemporaryPath path;
    auto bpm = MakeBufferPool(path.String());
    Executor executor(bpm);
    for (int key = 1; key <= 2000; ++key) {
        ASSERT_EQ(executor.ExecuteQuery("INSERT " + std::to_string(key) + " " +
                                        std::to_string(key * 100)),
                  "Inserted (" + std::to_string(key) + ", " +
                      std::to_string(key * 100) + ")");
    }

    constexpr uint32_t seed = 20261030;
    std::cout << "TSan stress seed: " << seed << std::endl;
    std::atomic<int> errors{0};
    std::vector<std::thread> readers;
    for (int reader_id = 0; reader_id < 3; ++reader_id) {
        readers.emplace_back([&, reader_id] {
            std::mt19937 random(seed + static_cast<uint32_t>(reader_id));
            std::uniform_int_distribution<int> key_distribution(1, 2000);
            for (int lookup = 0; lookup < 20000; ++lookup) {
                const int key = key_distribution(random);
                if (executor.ExecuteQuery("SELECT " + std::to_string(key)) !=
                    "Result: " + std::to_string(key * 100)) {
                    ++errors;
                }
            }
        });
    }

    std::thread writer([&] {
        for (int key = 2001; key <= 4000; ++key) {
            try {
                executor.ExecuteQuery("INSERT " + std::to_string(key) + " " +
                                      std::to_string(key * 100));
            } catch (const DbError&) {
                ++errors;
            }
        }
    });
    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(executor.ExecuteQuery("SELECT 4000"), "Result: 400000");
}

TEST(SqlTest, DISABLED_DocumentedExamplesWork) {
    const std::string command = "bash '" + std::string(RELDB_SOURCE_DIR) +
                                "/scripts/check_documented_sql.sh' '" +
                                std::string(RELDB_SERVER_BINARY) + "'";
    ASSERT_EQ(std::system(command.c_str()), 0)
        << "The documented SQL examples failed against the live Crow server.";
}

TEST(MetricsTest, NoFabricatedValues) {
    const std::vector<std::filesystem::path> scanned_roots = {
        std::filesystem::path(RELDB_SOURCE_DIR) / "src",
        std::filesystem::path(RELDB_SOURCE_DIR) / "include",
        std::filesystem::path(RELDB_SOURCE_DIR) / "dashboard",
    };

    for (const auto& root : scanned_roots) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::ifstream source(entry.path());
            const std::string content((std::istreambuf_iterator<char>(source)),
                                      std::istreambuf_iterator<char>());
            EXPECT_EQ(content.find("rand("), std::string::npos) << entry.path();
            EXPECT_EQ(content.find("Math.random"), std::string::npos) << entry.path();
        }
    }
}
