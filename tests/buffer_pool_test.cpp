#include "buffer_pool_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "clock_replacer.h"
#include "crc32c.h"
#include "db_error.h"
#include "disk_manager.h"

namespace {

using relational_db::BasicPageGuard;
using relational_db::BufferPoolManager;
using relational_db::DbError;
using relational_db::ErrorCode;
using relational_db::FileDiskManager;
using relational_db::PAGE_DATA_SIZE;
using PageBody = std::array<char, PAGE_DATA_SIZE>;

class MemoryDiskManager final : public relational_db::IDiskManager {
   public:
    MemoryDiskManager() {
        std::fill(pages_[0].begin() + 24, pages_[0].begin() + 28, static_cast<char>(0xff));
    }

    void ReadPage(relational_db::page_id_t page_id, char* page_data) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::memcpy(page_data, pages_[static_cast<size_t>(page_id)].data(), PAGE_DATA_SIZE);
    }

    void WritePage(relational_db::page_id_t page_id, const char* page_data,
                   relational_db::PageType = relational_db::PageType::HEAP,
                   uint16_t = 0, uint64_t = 0) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::memcpy(pages_[static_cast<size_t>(page_id)].data(), page_data, PAGE_DATA_SIZE);
    }

    void WriteHeaderPage(const char* page_data) override {
        WritePage(0, page_data, relational_db::PageType::HEADER);
    }

    void Sync() override {}
    uint64_t SizeInPages() const override { return pages_.size(); }
    void Extend(uint64_t) override {}
    relational_db::DiskStats Stats() const override { return {}; }

   private:
    std::array<PageBody, 129> pages_{};
    std::mutex mutex_;
};

class ToggleWriteBackPolicy final : public relational_db::WriteBackPolicy {
   public:
    bool CanWriteBack(relational_db::page_id_t) const override {
        return allowed.load(std::memory_order_relaxed);
    }

    void BeforeWriteBack(relational_db::page_id_t page_id, uint64_t lsn) override {
        std::lock_guard<std::mutex> lock(mutex);
        pages.push_back(page_id);
        lsns.push_back(lsn);
    }

    std::atomic<bool> allowed{false};
    std::mutex mutex;
    std::vector<relational_db::page_id_t> pages;
    std::vector<uint64_t> lsns;
};

class BlockingDiskManager final : public relational_db::IDiskManager {
   public:
    explicit BlockingDiskManager(std::shared_ptr<relational_db::IDiskManager> inner)
        : inner_(std::move(inner)) {}

    void BlockNextPageOneWrite() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_page_one_ = true;
        entered_ = false;
        released_ = false;
    }

    void WaitForBlockedWrite() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&]() { return entered_; });
    }

    void ReleaseBlockedWrite() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

    void ReadPage(relational_db::page_id_t page_id, char* page_data) override {
        inner_->ReadPage(page_id, page_data);
    }

    void WritePage(relational_db::page_id_t page_id, const char* page_data,
                   relational_db::PageType type = relational_db::PageType::HEAP,
                   uint16_t flags = 0, uint64_t lsn = 0) override {
        if (page_id == 1) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (block_page_one_) {
                block_page_one_ = false;
                entered_ = true;
                condition_.notify_all();
                condition_.wait(lock, [&]() { return released_; });
            }
        }
        inner_->WritePage(page_id, page_data, type, flags, lsn);
    }

    void WriteHeaderPage(const char* page_data) override {
        inner_->WriteHeaderPage(page_data);
    }
    void Sync() override { inner_->Sync(); }
    uint64_t SizeInPages() const override { return inner_->SizeInPages(); }
    void Extend(uint64_t page_count) override { inner_->Extend(page_count); }
    relational_db::DiskStats Stats() const override { return inner_->Stats(); }

   private:
    std::shared_ptr<relational_db::IDiskManager> inner_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool block_page_one_{false};
    bool entered_{false};
    bool released_{false};
};

class FailingReadDiskManager final : public relational_db::IDiskManager {
   public:
    explicit FailingReadDiskManager(std::shared_ptr<relational_db::IDiskManager> inner)
        : inner_(std::move(inner)) {}

    void FailNextRead(relational_db::page_id_t page_id) {
        fail_page_id_.store(page_id, std::memory_order_release);
    }

    void ReadPage(relational_db::page_id_t page_id, char* page_data) override {
        if (fail_page_id_.exchange(relational_db::INVALID_PAGE_ID, std::memory_order_acq_rel) ==
            page_id) {
            std::fill(page_data, page_data + PAGE_DATA_SIZE, '!');
            throw DbError(ErrorCode::IO_ERROR, "Injected page-read failure", page_id);
        }
        inner_->ReadPage(page_id, page_data);
    }
    void WritePage(relational_db::page_id_t page_id, const char* page_data,
                   relational_db::PageType type = relational_db::PageType::HEAP,
                   uint16_t flags = 0, uint64_t lsn = 0) override {
        inner_->WritePage(page_id, page_data, type, flags, lsn);
    }
    void WriteHeaderPage(const char* page_data) override { inner_->WriteHeaderPage(page_data); }
    void Sync() override { inner_->Sync(); }
    uint64_t SizeInPages() const override { return inner_->SizeInPages(); }
    void Extend(uint64_t page_count) override { inner_->Extend(page_count); }
    relational_db::DiskStats Stats() const override { return inner_->Stats(); }

   private:
    std::shared_ptr<relational_db::IDiskManager> inner_;
    std::atomic<relational_db::page_id_t> fail_page_id_{relational_db::INVALID_PAGE_ID};
};

struct StressRecord {
    uint32_t magic;
    uint32_t page_id;
    uint32_t sequence_low;
    uint32_t sequence_high;
    uint32_t crc;
};

static_assert(sizeof(StressRecord) <= PAGE_DATA_SIZE);

class TemporaryDatabase {
   public:
    TemporaryDatabase() {
        static std::atomic<uint64_t> next_id{0};
        path_ = std::filesystem::temp_directory_path() /
                ("custom-db-buffer-pool-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(next_id.fetch_add(1)));
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    std::string String() const { return path_.string(); }

   private:
    std::filesystem::path path_;
};

class ScopedEnvironment {
   public:
    ScopedEnvironment(const char* name, const char* value) : name_(name) {
        const char* original = std::getenv(name);
        if (original != nullptr) {
            original_ = original;
        }
#ifdef _WIN32
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }

    ~ScopedEnvironment() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), original_ ? original_->c_str() : "");
#else
        if (original_) {
            setenv(name_.c_str(), original_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

   private:
    std::string name_;
    std::optional<std::string> original_;
};

}  // namespace

TEST(BufferPoolGuardTest, GuardsOwnPinsAndMoveWithoutLeaking) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    BufferPoolManager pool(16, disk);
    {
        auto guard = pool.FetchBasic(0);
        EXPECT_EQ(guard.GetPageId(), 0);
        EXPECT_EQ(pool.pinned_frames(), 1U);
        const auto snapshot = pool.Snapshot();
        ASSERT_EQ(snapshot.size(), 16U);
        const auto& frame = snapshot[static_cast<size_t>(guard.GetFrameId())];
        EXPECT_EQ(frame.page_id, 0);
        EXPECT_EQ(frame.pin_count, 1);
        auto moved = std::move(guard);
        EXPECT_FALSE(guard.IsValid());
        EXPECT_TRUE(moved.IsValid());
    }

    EXPECT_EQ(pool.pinned_frames(), 0U);
    EXPECT_NO_THROW(pool.AssertNoPinsHeld());
}

TEST(BufferPoolConfigTest, ValidatesFrameCountAndPolicyEnvironment) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    {
        ScopedEnvironment frames("BUFFER_POOL_FRAMES", "15");
        try {
            BufferPoolManager invalid(disk);
            FAIL() << "Expected invalid frame-count configuration";
        } catch (const DbError& error) {
            EXPECT_EQ(error.Code(), ErrorCode::INVALID_ARGUMENT);
        }
    }
    {
        ScopedEnvironment frames("BUFFER_POOL_FRAMES", "16");
        BufferPoolManager valid(disk);
        EXPECT_EQ(valid.PoolSize(), 16U);
    }
    {
        ScopedEnvironment policy("BUFFER_POOL_POLICY", "invalid");
        try {
            BufferPoolManager invalid(16, disk);
            FAIL() << "Expected invalid replacement-policy configuration";
        } catch (const DbError& error) {
            EXPECT_EQ(error.Code(), ErrorCode::INVALID_ARGUMENT);
        }
    }
}

TEST(BufferPoolManagerTest, DirtyVictimIsWrittenBackBeforeReuse) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    disk->Extend(20);
    BufferPoolManager pool(16, disk);

    {
        auto guard = pool.FetchWrite(1);
        auto* body = guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        body->fill('x');
    }
    for (relational_db::page_id_t page_id = 2; page_id <= 16; ++page_id) {
        auto guard = pool.FetchRead(page_id);
    }
    {
        auto guard = pool.FetchRead(17);
    }

    EXPECT_GE(pool.SnapshotStats().dirty_write_backs, 1U);
    auto persisted = pool.FetchRead(1);
    EXPECT_EQ(persisted.As<PageBody>()->front(), 'x');
    persisted.Drop();
    pool.AssertNoPinsHeld();
}

TEST(BufferPoolManagerTest, ConcurrentFetchesOfSamePageShareOneFrame) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    disk->Extend(1);
    BufferPoolManager pool(16, disk);
    const uint64_t misses_before = pool.SnapshotStats().misses;
    constexpr size_t thread_count = 16;
    constexpr uint32_t seed = 20261030;
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<relational_db::frame_id_t> frames(thread_count, -1);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&, index]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto guard = pool.FetchRead(1);
            frames[index] = guard.GetFrameId();
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    std::cout << "Same-page concurrent fetch seed=" << seed << '\n';
    for (const auto frame_id : frames) {
        EXPECT_EQ(frame_id, frames.front());
    }
    EXPECT_EQ(pool.SnapshotStats().misses, misses_before + 1U);
    EXPECT_NO_THROW(pool.AssertNoPinsHeld());
}

TEST(BufferPoolManagerTest, RefetchWaitsForDirtyVictimWriteBack) {
    TemporaryDatabase database;
    auto file_disk = std::make_shared<FileDiskManager>(database.String());
    file_disk->Extend(20);
    auto disk = std::make_shared<BlockingDiskManager>(file_disk);
    BufferPoolManager pool(16, disk);
    {
        auto guard = pool.FetchWrite(1);
        guard.AsMut<PageBody>()->fill('v');
    }
    for (relational_db::page_id_t page_id = 2; page_id <= 16; ++page_id) {
        auto guard = pool.FetchRead(page_id);
    }

    disk->BlockNextPageOneWrite();
    std::thread eviction([&]() {
        auto guard = pool.FetchRead(17);
    });
    disk->WaitForBlockedWrite();

    std::atomic<bool> refetch_done{false};
    char refetched_value = '\0';
    std::thread refetch([&]() {
        auto guard = pool.FetchRead(1);
        refetched_value = guard.As<PageBody>()->front();
        refetch_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(refetch_done.load(std::memory_order_acquire));

    disk->ReleaseBlockedWrite();
    eviction.join();
    refetch.join();
    EXPECT_EQ(refetched_value, 'v');
    EXPECT_NO_THROW(pool.AssertNoPinsHeld());
}

TEST(BufferPoolManagerTest, FailedReplacementReadRestoresVictimContents) {
    TemporaryDatabase database;
    auto file_disk = std::make_shared<FileDiskManager>(database.String());
    file_disk->Extend(20);
    auto disk = std::make_shared<FailingReadDiskManager>(file_disk);
    BufferPoolManager pool(16, disk);
    {
        auto guard = pool.FetchWrite(1);
        guard.AsMut<PageBody>()->fill('x');
    }
    for (relational_db::page_id_t page_id = 2; page_id <= 16; ++page_id) {
        auto guard = pool.FetchRead(page_id);
    }

    disk->FailNextRead(17);
    try {
        auto failed = pool.FetchRead(17);
        FAIL() << "Expected injected page-read failure";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::IO_ERROR);
    }
    auto restored = pool.FetchRead(1);
    EXPECT_EQ(restored.As<PageBody>()->front(), 'x');
    restored.Drop();
    EXPECT_NO_THROW(pool.AssertNoPinsHeld());
}

TEST(BufferPoolManagerTest, ThrowsTypedErrorWhenAllFramesArePinned) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    disk->Extend(20);
    BufferPoolManager pool(16, disk);
    std::vector<BasicPageGuard> pinned;
    pinned.reserve(16);
    for (relational_db::page_id_t page_id = 1; page_id <= 16; ++page_id) {
        pinned.push_back(pool.FetchBasic(page_id));
    }

    try {
        auto exhausted = pool.FetchRead(17);
        FAIL() << "Expected buffer-pool exhaustion";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::BUFFER_POOL_EXHAUSTED);
    }
    pinned.clear();
    EXPECT_NO_THROW(pool.AssertNoPinsHeld());
}

TEST(PageAllocatorTest, DeletePageReturnsPageToGuardManagedFreeList) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    BufferPoolManager pool(16, disk);
    {
        auto page = pool.NewPageGuarded();
        ASSERT_EQ(page.GetPageId(), 1);
        page.AsMut<std::array<char, PAGE_DATA_SIZE>>()->fill('z');
    }

    ASSERT_TRUE(pool.DeletePage(1));
    auto reused = pool.NewPageGuarded();
    EXPECT_EQ(reused.GetPageId(), 1);
    EXPECT_EQ(reused.As<PageBody>()->front(), '\0');
    reused.Drop();
    pool.FlushAll();
    EXPECT_NO_THROW(pool.AssertNoPinsHeld());
}

TEST(PageAllocatorTest, FreeListSurvivesReopening) {
    TemporaryDatabase database;
    {
        auto disk = std::make_shared<FileDiskManager>(database.String());
        BufferPoolManager pool(16, disk);
        auto page = pool.NewPageGuarded();
        ASSERT_EQ(page.GetPageId(), 1);
        page.Drop();
        ASSERT_TRUE(pool.DeletePage(1));
        pool.FlushAll();
    }

    auto disk = std::make_shared<FileDiskManager>(database.String());
    BufferPoolManager pool(16, disk);
    try {
        auto deleted = pool.FetchRead(1);
        FAIL() << "Expected deleted page to remain unavailable after reopening";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::PAGE_OUT_OF_RANGE);
    }
    auto reused = pool.NewPageGuarded();
    EXPECT_EQ(reused.GetPageId(), 1);
    reused.Drop();
    EXPECT_NO_THROW(pool.AssertNoPinsHeld());
}

TEST(BufferPoolManagerTest, FlushAllUsesPolicyAndSyncsAllowedPages) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    disk->Extend(1);
    BufferPoolManager pool(16, disk);
    auto policy = std::make_shared<ToggleWriteBackPolicy>();
    pool.SetWriteBackPolicy(policy);
    {
        auto page = pool.FetchWrite(1);
        page.AsMut<PageBody>()->front() = 'w';
    }

    const auto before_denied = disk->Stats();
    pool.FlushAll();
    const auto after_denied = disk->Stats();
    EXPECT_EQ(after_denied.writes, before_denied.writes);
    EXPECT_EQ(after_denied.syncs, before_denied.syncs + 1U);
    EXPECT_EQ(pool.SnapshotStats().dirty_write_backs, 0U);

    policy->allowed.store(true, std::memory_order_relaxed);
    pool.FlushAll();
    EXPECT_EQ(pool.SnapshotStats().dirty_write_backs, 1U);
    EXPECT_EQ(policy->pages, (std::vector<relational_db::page_id_t>{1}));
    EXPECT_EQ(policy->lsns, (std::vector<uint64_t>{0}));
}

TEST(ClockReplacerTest, EvictsFromClockOrderWithoutCountingScans) {
    relational_db::ClockReplacer replacer(3);
    replacer.Unpin(0);
    replacer.Unpin(1);
    replacer.Unpin(2);
    EXPECT_EQ(replacer.Size(), 3U);
    relational_db::frame_id_t victim = -1;
    ASSERT_TRUE(replacer.Victim(&victim));
    EXPECT_EQ(victim, 0);
    EXPECT_EQ(replacer.Size(), 2U);
}

TEST(LruKReplacerTest, PrefersColdFramesOverRepeatedlyUsedFrames) {
    relational_db::LruKReplacer replacer(3, 2);
    replacer.Unpin(0);
    replacer.Unpin(1);
    replacer.Pin(1);
    replacer.Unpin(1);
    replacer.Unpin(2);
    replacer.Pin(2);
    replacer.Unpin(2);
    relational_db::frame_id_t victim = -1;
    ASSERT_TRUE(replacer.Victim(&victim));
    EXPECT_EQ(victim, 0);
}

TEST(BufferPoolStressTest, SixteenThreadsOneMillionVerifiedOperations) {
    auto disk = std::make_shared<MemoryDiskManager>();
    BufferPoolManager pool(64, disk);
    constexpr size_t thread_count = 16;
    constexpr uint32_t operation_count = 1000000;
    constexpr uint32_t seed = 20261030;
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (uint32_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&, thread_index]() {
            for (uint32_t operation = 0; operation < operation_count; ++operation) {
                const uint32_t page_id =
                    1U + ((operation * 17U + thread_index * 31U) % 128U);
                auto guard = pool.FetchWrite(static_cast<relational_db::page_id_t>(page_id));
                auto* record = guard.AsMut<StressRecord>();
                if (record->magic == 0xCDBF00D1U &&
                    (record->page_id != page_id ||
                     record->crc != relational_db::Crc32c(
                                        reinterpret_cast<const uint8_t*>(record),
                                        offsetof(StressRecord, crc)))) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }

                const uint64_t sequence =
                    static_cast<uint64_t>(thread_index) * operation_count + operation;
                record->magic = 0xCDBF00D1U;
                record->page_id = page_id;
                record->sequence_low = static_cast<uint32_t>(sequence);
                record->sequence_high = static_cast<uint32_t>(sequence >> 32U);
                record->crc = relational_db::Crc32c(
                    reinterpret_cast<const uint8_t*>(record), offsetof(StressRecord, crc));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::cout << "Buffer-pool stress seed=" << seed << " operations_per_thread="
              << operation_count << '\n';
    EXPECT_FALSE(failed.load(std::memory_order_relaxed)) << "seed=" << seed;
    EXPECT_NO_THROW(pool.AssertNoPinsHeld());
}
