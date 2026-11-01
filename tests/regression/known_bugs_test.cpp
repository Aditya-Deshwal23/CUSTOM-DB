#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "b_plus_tree.h"
#include "disk_manager.h"
#include "executor.h"
#include "wal.h"

#ifndef _WIN32
#include <fcntl.h>
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
using relational_db::WriteAheadLog;

#if defined(__linux__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAddressSanitizerEnabled = true;
#else
constexpr bool kAddressSanitizerEnabled = false;
#endif
#elif defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizerEnabled = true;
#else
constexpr bool kAddressSanitizerEnabled = false;
#endif
#endif

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
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-wal.tmp", error);
        std::filesystem::remove(path_.string() + ".ack", error);
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
    ASSERT_TRUE(result) << "Key 5 not found after reopening the same database file.";
    ASSERT_EQ(*result, 500);
}

TEST(PersistenceTest, ExecutorIndexAndRowsSurviveCleanRestart) {
    TemporaryPath path;
    {
        auto disk = std::make_shared<DiskManager>(path.String());
        WriteAheadLog wal(path.String());
        wal.Recover(*disk);
        auto bpm = std::make_shared<BufferPoolManager>(50, disk);
        Executor executor(bpm, wal);
        EXPECT_EQ(executor.ExecuteQuery("insert 42 9001;"), "Inserted (42, 9001)");
        EXPECT_EQ(executor.ExecuteQuery("sElEcT 42"), "Result: 9001");
        EXPECT_FALSE(executor.IntegrityCheck());
    }

    auto disk = std::make_shared<DiskManager>(path.String());
    WriteAheadLog wal(path.String());
    wal.Recover(*disk);
    auto bpm = std::make_shared<BufferPoolManager>(50, disk);
    Executor reopened_executor(bpm, wal);
    EXPECT_EQ(reopened_executor.ExecuteQuery("select 42;"), "Result: 9001");
    EXPECT_FALSE(reopened_executor.IntegrityCheck());
}

TEST(WalTest, RollbackRestoresPageImagesAndAllocationState) {
    TemporaryPath path;
    auto disk = std::make_shared<DiskManager>(path.String());
    WriteAheadLog wal(path.String());
    wal.Recover(*disk);
    auto bpm = std::make_shared<BufferPoolManager>(16, disk);

    const uint64_t page_count_before = disk->SizeInPages();
    auto transaction = bpm->BeginTransaction(wal);
    relational_db::page_id_t allocated_page = relational_db::INVALID_PAGE_ID;
    {
        relational_db::WritePageGuard page = bpm->NewPageGuarded();
        allocated_page = page.GetPageId();
        page.SetPageType(relational_db::PageType::BTREE_LEAF);
        auto* body = page.AsMut<std::array<char, relational_db::PAGE_DATA_SIZE>>();
        body->fill('x');
    }

    const uint64_t writes_before_flush = disk->Stats().writes;
    {
        relational_db::WritePageGuard header = bpm->FetchWrite(0);
        auto* body = header.AsMut<std::array<char, relational_db::PAGE_DATA_SIZE>>();
        body->back() = 'x';
    }
    EXPECT_TRUE(bpm->FlushPage(0));
    EXPECT_EQ(disk->Stats().writes, writes_before_flush);
    transaction->Rollback();

    EXPECT_EQ(disk->SizeInPages(), page_count_before);
    EXPECT_THROW(bpm->FetchRead(allocated_page), DbError);
    auto retry = bpm->BeginTransaction(wal);
    {
        relational_db::WritePageGuard page = bpm->NewPageGuarded();
        EXPECT_EQ(page.GetPageId(), allocated_page);
        EXPECT_EQ(page.GetPageType(), relational_db::PageType::HEAP);
        const auto* body = page.As<std::array<char, relational_db::PAGE_DATA_SIZE>>();
        EXPECT_TRUE(std::all_of(body->begin(), body->end(), [](char value) { return value == 0; }));
    }
    retry->Rollback();
}

TEST(WalTest, StatementThatExceedsBufferPoolRollsBack) {
    TemporaryPath path;
    auto disk = std::make_shared<DiskManager>(path.String());
    WriteAheadLog wal(path.String());
    wal.Recover(*disk);
    auto bpm = std::make_shared<BufferPoolManager>(16, disk);
    auto transaction = bpm->BeginTransaction(wal);
    constexpr size_t kAllocationWorkers = 17;
    std::mutex release_mutex;
    std::condition_variable release_condition;
    bool release_guards = false;
    std::atomic<size_t> successful_allocations{0};
    std::atomic<bool> allocation_failed{false};
    std::atomic<int> failure_code{static_cast<int>(relational_db::ErrorCode::INVALID_ARGUMENT)};
    std::vector<std::thread> allocation_workers;
    allocation_workers.reserve(kAllocationWorkers);

    for (size_t worker = 0; worker < kAllocationWorkers; ++worker) {
        allocation_workers.emplace_back([&]() {
            try {
                relational_db::WritePageGuard guard = bpm->NewPageGuarded();
                successful_allocations.fetch_add(1, std::memory_order_release);
                std::unique_lock<std::mutex> lock(release_mutex);
                release_condition.wait(lock, [&]() { return release_guards; });
            } catch (const DbError& error) {
                failure_code.store(static_cast<int>(error.Code()), std::memory_order_release);
                allocation_failed.store(true, std::memory_order_release);
            }
        });
    }

    while (!allocation_failed.load(std::memory_order_acquire) &&
           successful_allocations.load(std::memory_order_acquire) < kAllocationWorkers) {
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> lock(release_mutex);
        release_guards = true;
    }
    release_condition.notify_all();
    for (std::thread& worker : allocation_workers) {
        worker.join();
    }

    EXPECT_TRUE(allocation_failed.load(std::memory_order_acquire));
    EXPECT_EQ(failure_code.load(std::memory_order_acquire),
              static_cast<int>(relational_db::ErrorCode::TXN_TOO_LARGE));
    transaction->Rollback();
    EXPECT_EQ(disk->SizeInPages(), 1U);
    EXPECT_NO_THROW(bpm->AssertNoPinsHeld());
}

TEST(WalTest, RecoveryReplaysCommittedInsertUpdateAndDeleteStatements) {
    TemporaryPath path;
    {
        auto disk = std::make_shared<DiskManager>(path.String());
        WriteAheadLog wal(path.String());
        wal.Recover(*disk);
        auto bpm = std::make_shared<BufferPoolManager>(32, disk);
        auto insert = bpm->BeginTransaction(wal);
        BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
        for (int batch = 1; batch <= 2; ++batch) {
            for (int index = 0; index < 4; ++index) {
                ASSERT_TRUE(tree.Insert(batch * 1000 + index, 4));
            }
        }
        insert->Commit();

        auto update = bpm->BeginTransaction(wal);
        for (int index = 0; index < 4; ++index) {
            const int key = 1000 + index;
            ASSERT_TRUE(tree.Delete(key));
            ASSERT_TRUE(tree.Insert(key, 5));
        }
        update->Commit();

        auto remove = bpm->BeginTransaction(wal);
        for (int index = 0; index < 4; ++index) {
            ASSERT_TRUE(tree.Delete(2000 + index));
        }
        remove->Commit();
    }

    auto disk = std::make_shared<DiskManager>(path.String(), true);
    WriteAheadLog wal(path.String());
    EXPECT_EQ(wal.Recover(*disk).commits, 3U);
    auto bpm = std::make_shared<BufferPoolManager>(32, disk);
    BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
    for (int index = 0; index < 4; ++index) {
        EXPECT_EQ(tree.GetValue(1000 + index), 5);
        EXPECT_FALSE(tree.GetValue(2000 + index));
    }
    EXPECT_FALSE(tree.Validate());
}

TEST(WalTest, CheckpointExcludesNewWriteTransactionsUntilTruncate) {
#if defined(_WIN32)
    GTEST_SKIP() << "Failpoint environment mutation test is not enabled on Windows.";
#else
    TemporaryPath path;
    auto disk = std::make_shared<DiskManager>(path.String());
    WriteAheadLog wal(path.String());
    wal.Recover(*disk);
    auto bpm = std::make_shared<BufferPoolManager>(16, disk);
    auto transaction = bpm->BeginTransaction(wal);
    {
        relational_db::WritePageGuard page = bpm->FetchWrite(0);
        auto* body = page.AsMut<std::array<char, relational_db::PAGE_DATA_SIZE>>();
        body->back() = 'x';
    }
    transaction->Commit();

    setenv("RELDB_FAILPOINT", "checkpoint_before_truncate:throw", 1);
    EXPECT_THROW(wal.Checkpoint(*bpm), DbError);
    unsetenv("RELDB_FAILPOINT");
    EXPECT_NO_THROW(bpm->BeginTransaction(wal)->Rollback());
#endif
}

TEST(WalTest, AutomaticCheckpointFailureDoesNotFailCommittedStatement) {
#if defined(_WIN32)
    GTEST_SKIP() << "Failpoint environment mutation test is not enabled on Windows.";
#else
    TemporaryPath path;
    relational_db::page_id_t committed_page_id = relational_db::INVALID_PAGE_ID;
    {
        auto disk = std::make_shared<DiskManager>(path.String());
        WriteAheadLog wal(path.String(), relational_db::DurabilityMode::FULL, 1000, 1);
        wal.Recover(*disk);
        auto bpm = std::make_shared<BufferPoolManager>(16, disk);
        auto transaction = bpm->BeginTransaction(wal);
        {
            auto page = bpm->NewPageGuarded();
            committed_page_id = page.GetPageId();
            page.AsMut<std::array<char, relational_db::PAGE_DATA_SIZE>>()->fill('x');
        }

        ASSERT_EQ(setenv("RELDB_FAILPOINT", "checkpoint_before_truncate:throw", 1), 0);
        EXPECT_NO_THROW(transaction->Commit());
        EXPECT_THROW(wal.Checkpoint(*bpm), DbError);
        unsetenv("RELDB_FAILPOINT");
    }

    auto disk = std::make_shared<DiskManager>(path.String());
    WriteAheadLog wal(path.String());
    wal.Recover(*disk);
    auto bpm = std::make_shared<BufferPoolManager>(16, disk);
    auto page = bpm->FetchRead(committed_page_id);
    const auto* body = page.As<std::array<char, relational_db::PAGE_DATA_SIZE>>();
    EXPECT_EQ(body->front(), 'x');
#endif
}

TEST(WalTest, EveryCommittedStatementIsDurableBeforeReturning) {
    const std::array<relational_db::DurabilityMode, 3> modes{relational_db::DurabilityMode::FULL,
                                                             relational_db::DurabilityMode::NORMAL,
                                                             relational_db::DurabilityMode::OFF};
    for (const relational_db::DurabilityMode mode : modes) {
        TemporaryPath path;
        auto disk = std::make_shared<DiskManager>(path.String());
        WriteAheadLog wal(path.String(), mode, 60000, 16U * 1024U * 1024U);
        wal.Recover(*disk);
        auto bpm = std::make_shared<BufferPoolManager>(32, disk);
        Executor executor(bpm, wal);
        EXPECT_EQ(executor.ExecuteQuery("INSERT 8 80"), "Inserted (8, 80)");

        const uint64_t committed_lsn = wal.GetNextLSN();
        EXPECT_GE(wal.GetFirstUnsyncedLSN(), committed_lsn);
        bpm->FlushAll();
        EXPECT_GE(wal.GetFirstUnsyncedLSN(), committed_lsn);

        auto recovered_disk = std::make_shared<DiskManager>(path.String(), true);
        WriteAheadLog recovered_wal(path.String());
        recovered_wal.Recover(*recovered_disk);
        auto recovered_bpm = std::make_shared<BufferPoolManager>(32, recovered_disk);
        Executor recovered_executor(recovered_bpm, recovered_wal);
        EXPECT_EQ(recovered_executor.ExecuteQuery("SELECT 8"), "Result: 80");
    }
}

TEST(WalTest, NormalModeSyncThreadTsanClean) {
    TemporaryPath path;
    constexpr uint32_t seed = 20261030;
    std::cout << "WAL sync-thread stress seed: " << seed << std::endl;
    std::mt19937 random(seed);
    std::vector<int> keys(128);
    for (size_t index = 0; index < keys.size(); ++index) {
        keys[index] = static_cast<int>(index + 1U);
    }
    std::shuffle(keys.begin(), keys.end(), random);

    auto disk = std::make_shared<DiskManager>(path.String());
    WriteAheadLog wal(path.String(), relational_db::DurabilityMode::NORMAL, 1, 16U * 1024U * 1024U);
    wal.Recover(*disk);
    auto bpm = std::make_shared<BufferPoolManager>(32, disk);
    Executor executor(bpm, wal);
    for (const int key : keys) {
        EXPECT_EQ(
            executor.ExecuteQuery("INSERT " + std::to_string(key) + " " + std::to_string(key * 10)),
            "Inserted (" + std::to_string(key) + ", " + std::to_string(key * 10) + ")");
    }
    const uint64_t pending_lsn = wal.GetNextLSN();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_GE(wal.GetFirstUnsyncedLSN(), pending_lsn);
    bpm->FlushAll();
    EXPECT_FALSE(executor.IntegrityCheck());
}

TEST(WalTest, RecoveryCanBeInterruptedAndRetried) {
#if !defined(__linux__)
    GTEST_SKIP() << "Recovery kill test requires Linux process semantics.";
#else
    TemporaryPath path;
    {
        auto disk = std::make_shared<DiskManager>(path.String());
        WriteAheadLog wal(path.String());
        wal.Recover(*disk);
        auto bpm = std::make_shared<BufferPoolManager>(32, disk);
        Executor executor(bpm, wal);
        EXPECT_EQ(executor.ExecuteQuery("INSERT 91 910"), "Inserted (91, 910)");
    }

    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        try {
            auto disk = std::make_shared<DiskManager>(path.String(), true);
            WriteAheadLog wal(path.String());
            setenv("RELDB_FAILPOINT", "recovery_mid:kill", 1);
            wal.Recover(*disk);
        } catch (const std::exception&) {
            _exit(21);
        }
        _exit(22);
    }
    int child_status = 0;
    ASSERT_EQ(waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(child_status));
    ASSERT_EQ(WTERMSIG(child_status), SIGKILL);

    auto disk = std::make_shared<DiskManager>(path.String(), true);
    WriteAheadLog wal(path.String());
    const auto recovery = wal.Recover(*disk);
    EXPECT_GT(recovery.pages_replayed, 0U);
    EXPECT_EQ(wal.Recover(*disk).pages_replayed, 0U);
    auto bpm = std::make_shared<BufferPoolManager>(32, disk);
    Executor executor(bpm, wal);
    EXPECT_EQ(executor.ExecuteQuery("SELECT 91"), "Result: 910");
#endif
}

TEST(WalTest, CommitAppendCrashIsAtomic) {
#if !defined(__linux__)
    GTEST_SKIP() << "Commit append crash test requires Linux process semantics.";
#else
    TemporaryPath path;
    {
        auto disk = std::make_shared<DiskManager>(path.String());
        WriteAheadLog wal(path.String());
        wal.Recover(*disk);
        auto bpm = std::make_shared<BufferPoolManager>(32, disk);
        auto initialization = bpm->BeginTransaction(wal);
        BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
        initialization->Commit();
    }

    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        try {
            auto disk = std::make_shared<DiskManager>(path.String(), true);
            WriteAheadLog wal(path.String());
            wal.Recover(*disk);
            auto bpm = std::make_shared<BufferPoolManager>(32, disk);
            BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
            setenv("RELDB_FAILPOINT", "commit_after:kill", 1);
            auto transaction = bpm->BeginTransaction(wal);
            for (int index = 0; index < 4; ++index) {
                if (!tree.Insert(90000 + index, 4)) {
                    _exit(24);
                }
            }
            transaction->Commit();
        } catch (const std::exception&) {
            _exit(25);
        }
        _exit(26);
    }
    int child_status = 0;
    ASSERT_EQ(waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(child_status));
    ASSERT_EQ(WTERMSIG(child_status), SIGKILL);

    auto disk = std::make_shared<DiskManager>(path.String(), true);
    WriteAheadLog wal(path.String());
    wal.Recover(*disk);
    auto bpm = std::make_shared<BufferPoolManager>(32, disk);
    BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
    int present = 0;
    for (int index = 0; index < 4; ++index) {
        if (tree.GetValue(90000 + index)) {
            ++present;
        }
    }
    EXPECT_TRUE(present == 0 || present == 4);
    EXPECT_FALSE(tree.Validate());
    EXPECT_EQ(wal.Recover(*disk).pages_replayed, 0U);
#endif
}

TEST(DurabilityTest, HardKillKeepsAckedWrites) {
#if !defined(__linux__)
    GTEST_SKIP() << "Hard-kill durability test requires Linux process semantics.";
#else
    TemporaryPath path;
    constexpr int kBatches = 1000;
    constexpr int kBatchSize = 4;
    constexpr int kMarkerBase = 1500000000;
    uint32_t seed = 20261030;
    if (const char* configured_seed = std::getenv("RELDB_CRASHTEST_SEED")) {
        seed = static_cast<uint32_t>(std::strtoul(configured_seed, nullptr, 10));
    }
    std::cout << "Crash harness seed: " << seed << std::endl;
    std::mt19937 random(seed);
    std::vector<int> batch_ids(kBatches);
    for (int batch = 0; batch < kBatches; ++batch) {
        batch_ids[static_cast<size_t>(batch)] = batch + 1;
    }
    std::shuffle(batch_ids.begin(), batch_ids.end(), random);
    std::uniform_int_distribution<int> delay_distribution(1, 25);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        const int ack_fd =
            open((path.String() + ".ack").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (ack_fd < 0) {
            _exit(17);
        }
        try {
            auto disk = std::make_shared<DiskManager>(path.String(), true);
            WriteAheadLog wal(path.String());
            wal.Recover(*disk);
            auto bpm = std::make_shared<BufferPoolManager>(32, disk);
            auto initialization = bpm->BeginTransaction(wal);
            BPlusTree<int, int64_t, std::less<int>> tree(bpm, std::less<int>());
            initialization->Commit();
            uint64_t statement_sequence = 0;
            const auto commit_and_ack = [&](relational_db::TxnContext& transaction, char operation,
                                            int batch) {
                ++statement_sequence;
                const int operation_code = operation == 'I' ? 1 : (operation == 'U' ? 2 : 3);
                const int64_t marker_value = static_cast<int64_t>(operation_code * 1000000 + batch);
                if (!tree.Insert(kMarkerBase + static_cast<int>(statement_sequence),
                                 marker_value)) {
                    _exit(18);
                }
                transaction.Commit();
                const std::string ack = std::to_string(statement_sequence) + " " + operation + " " +
                                        std::to_string(batch) + "\n";
                if (write(ack_fd, ack.data(), ack.size()) != static_cast<ssize_t>(ack.size()) ||
                    fsync(ack_fd) != 0) {
                    _exit(19);
                }
            };
            for (const int batch : batch_ids) {
                auto transaction = bpm->BeginTransaction(wal);
                for (int index = 0; index < kBatchSize; ++index) {
                    if (!tree.Insert(batch * 1000 + index, kBatchSize)) {
                        _exit(18);
                    }
                }
                commit_and_ack(*transaction, 'I', batch);

                if (batch % 5 == 0) {
                    auto update = bpm->BeginTransaction(wal);
                    for (int index = 0; index < kBatchSize; ++index) {
                        const int key = batch * 1000 + index;
                        if (!tree.Delete(key) || !tree.Insert(key, kBatchSize + 1)) {
                            _exit(18);
                        }
                    }
                    commit_and_ack(*update, 'U', batch);
                }
                if (batch % 11 == 0) {
                    auto remove = bpm->BeginTransaction(wal);
                    for (int index = 0; index < kBatchSize; ++index) {
                        if (!tree.Delete(batch * 1000 + index)) {
                            _exit(18);
                        }
                    }
                    commit_and_ack(*remove, 'D', batch);
                }
            }
        } catch (...) {
            _exit(20);
        }
        close(ack_fd);
        _exit(0);
    }

    int child_status = 0;
    const auto ack_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool child_exited = false;
    while (std::chrono::steady_clock::now() < ack_deadline) {
        {
            std::ifstream ack_file(path.String() + ".ack");
            uint64_t sequence = 0;
            char operation = '\0';
            int batch = 0;
            if (ack_file >> sequence >> operation >> batch) {
                break;
            }
        }
        const pid_t result = waitpid(child, &child_status, WNOHANG);
        if (result == child) {
            child_exited = true;
            break;
        }
        ASSERT_TRUE(result == 0 || (result < 0 && errno == EINTR));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(std::filesystem::exists(path.String() + ".ack"));
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_distribution(random)));
    if (!child_exited) {
        const pid_t result = waitpid(child, &child_status, WNOHANG);
        if (result == 0) {
            ASSERT_EQ(kill(child, SIGKILL), 0);
            ASSERT_EQ(waitpid(child, &child_status, 0), child);
            ASSERT_TRUE(WIFSIGNALED(child_status));
            ASSERT_EQ(WTERMSIG(child_status), SIGKILL);
        } else {
            ASSERT_EQ(result, child);
            child_exited = true;
        }
    }
    if (child_exited) {
        ASSERT_TRUE(WIFEXITED(child_status));
        ASSERT_EQ(WEXITSTATUS(child_status), 0);
    }

    struct AcknowledgedStatement {
        uint64_t sequence;
        char operation;
        int batch;
    };
    std::vector<AcknowledgedStatement> acknowledged;
    {
        std::ifstream ack_file(path.String() + ".ack");
        AcknowledgedStatement statement{};
        while (ack_file >> statement.sequence >> statement.operation >> statement.batch) {
            acknowledged.push_back(statement);
        }
    }
    ASSERT_FALSE(acknowledged.empty());

    auto disk = std::make_shared<DiskManager>(path.String(), true);
    WriteAheadLog wal(path.String());
    wal.Recover(*disk);
    disk->ValidateHeader();
    auto bpm = std::make_shared<BufferPoolManager>(32, disk);
    BPlusTree<int, int64_t, std::less<int>> reopened_tree(bpm, std::less<int>());
    std::map<int, std::pair<int, int>> committed_batches;
    const auto markers = reopened_tree.RangeScan(kMarkerBase, kMarkerBase + 10000, true, true);
    for (const auto& marker : markers) {
        const uint64_t sequence = static_cast<uint64_t>(marker.first - kMarkerBase);
        const int operation_code = static_cast<int>(marker.second / 1000000);
        const int batch = static_cast<int>(marker.second % 1000000);
        committed_batches[batch] = {operation_code, static_cast<int>(sequence)};
    }
    for (const AcknowledgedStatement& statement : acknowledged) {
        const int operation_code =
            statement.operation == 'I' ? 1 : (statement.operation == 'U' ? 2 : 3);
        const auto marker =
            reopened_tree.GetValue(kMarkerBase + static_cast<int>(statement.sequence));
        ASSERT_TRUE(marker);
        EXPECT_EQ(*marker, operation_code * 1000000 + statement.batch);
    }
    for (int batch = 1; batch <= kBatches; ++batch) {
        int present = 0;
        for (int index = 0; index < kBatchSize; ++index) {
            const auto value = reopened_tree.GetValue(batch * 1000 + index);
            if (value) {
                ++present;
            }
        }
        EXPECT_TRUE(present == 0 || present == kBatchSize)
            << "batch " << batch << " was only partially recovered";
        const auto expected = committed_batches.find(batch);
        const int expected_present =
            expected == committed_batches.end() || expected->second.first == 3 ? 0 : kBatchSize;
        EXPECT_EQ(present, expected_present) << "batch " << batch << " disagrees with commit log";
        if (expected_present != 0) {
            const int expected_value = expected->second.first == 2 ? kBatchSize + 1 : kBatchSize;
            for (int index = 0; index < kBatchSize; ++index) {
                EXPECT_EQ(reopened_tree.GetValue(batch * 1000 + index), expected_value);
            }
        }
    }
    EXPECT_FALSE(reopened_tree.Validate());
    std::array<char, relational_db::PAGE_DATA_SIZE> verified_page{};
    for (uint64_t page_id = 0; page_id < disk->SizeInPages(); ++page_id) {
        disk->ReadPage(static_cast<relational_db::page_id_t>(page_id), verified_page.data());
    }
    bpm->AssertNoPinsHeld();
    const auto second_recovery = wal.Recover(*disk);
    EXPECT_EQ(second_recovery.pages_replayed, 0U);
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
        if constexpr (!kAddressSanitizerEnabled) {
            // ASan's shadow-memory mappings do not fit under this virtual-memory cap.
            struct rlimit memory_limit{};
            memory_limit.rlim_cur = 1024ULL * 1024ULL * 1024ULL;
            memory_limit.rlim_max = memory_limit.rlim_cur;
            if (setrlimit(RLIMIT_AS, &memory_limit) != 0) {
                _exit(20);
            }
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

TEST(WalTest, RecordAppendedAfterTornTailIsRecoverable) {
    TemporaryPath path;
    {
        auto disk = std::make_shared<DiskManager>(path.String());
        WriteAheadLog wal(path.String());
        wal.Recover(*disk);
        auto bpm = std::make_shared<BufferPoolManager>(32, disk);
        Executor executor(bpm, wal);
        ASSERT_EQ(executor.ExecuteQuery("INSERT 1 11"), "Inserted (1, 11)");
        ASSERT_EQ(executor.ExecuteQuery("INSERT 2 22"), "Inserted (2, 22)");
    }

    const std::string wal_path = path.String() + "-wal";
    const auto original_size = std::filesystem::file_size(wal_path);
    ASSERT_GT(original_size, 10U);
    std::filesystem::resize_file(wal_path, original_size - 10);
    {
        auto disk = std::make_shared<DiskManager>(path.String(), true);
        WriteAheadLog wal(path.String());
        const auto recovered = wal.Recover(*disk);
        EXPECT_EQ(recovered.commits, 2U);
        disk->ValidateHeader();
        auto bpm = std::make_shared<BufferPoolManager>(32, disk);
        Executor executor(bpm, wal);
        ASSERT_EQ(executor.ExecuteQuery("INSERT 3 33"), "Inserted (3, 33)");
    }

    auto disk = std::make_shared<DiskManager>(path.String(), true);
    WriteAheadLog recovered(path.String());
    const auto stats = recovered.Recover(*disk);
    EXPECT_EQ(stats.commits, 1U);
    disk->ValidateHeader();
    auto bpm = std::make_shared<BufferPoolManager>(32, disk);
    Executor executor(bpm, recovered);
    EXPECT_EQ(executor.ExecuteQuery("SELECT 1"), "Result: 11");
    EXPECT_EQ(executor.ExecuteQuery("SELECT 2"), "Key 2 not found.");
    EXPECT_EQ(executor.ExecuteQuery("SELECT 3"), "Result: 33");
}

TEST(ConcurrencyTest, ExecutorStatementGateTsanClean) {
    TemporaryPath path;
    auto disk = std::make_shared<DiskManager>(path.String());
    WriteAheadLog wal(path.String());
    wal.Recover(*disk);
    auto bpm = std::make_shared<BufferPoolManager>(64, disk);
    Executor executor(bpm, wal);
    for (int key = 1; key <= 2000; ++key) {
        ASSERT_EQ(executor.ExecuteQuery("INSERT " + std::to_string(key) + " " +
                                        std::to_string(key * 100)),
                  "Inserted (" + std::to_string(key) + ", " + std::to_string(key * 100) + ")");
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
            EXPECT_EQ(content.find("avg_latency / 3.0"), std::string::npos) << entry.path();
        }
    }
}
