#include "disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "buffer_pool_manager.h"
#include "crc32c.h"
#include "db_error.h"
#include "fault_injecting_disk_manager.h"

namespace {

using relational_db::Crc32c;
using relational_db::Crc32cSoftware;
using relational_db::DbError;
using relational_db::ErrorCode;
using relational_db::FileDiskManager;
using relational_db::PAGE_DATA_SIZE;
using relational_db::PAGE_SIZE;
using relational_db::testing::FaultInjectingDiskManager;

class TemporaryDatabase {
   public:
    TemporaryDatabase() {
        static std::atomic<uint64_t> next_id{0};
        path_ = std::filesystem::temp_directory_path() /
                ("custom-db-disk-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(next_id.fetch_add(1)));
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::string String() const {
        return path_.string();
    }

   private:
    std::filesystem::path path_;
};

void RewriteRawPage(const std::string& path, const std::vector<uint8_t>& image) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekp(PAGE_SIZE);
    file.write(reinterpret_cast<const char*>(image.data()),
               static_cast<std::streamsize>(image.size()));
    file.flush();
    ASSERT_TRUE(file.good());
}

void RewriteByte(const std::string& path, std::streamoff offset, uint8_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekp(offset);
    file.put(static_cast<char>(value));
    file.flush();
    ASSERT_TRUE(file.good());
}

std::vector<uint8_t> ReadRawPage(const std::string& path) {
    std::vector<uint8_t> image(PAGE_SIZE);
    std::ifstream file(path, std::ios::binary);
    file.seekg(PAGE_SIZE);
    file.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size()));
    EXPECT_EQ(file.gcount(), static_cast<std::streamsize>(image.size()));
    return image;
}

void ExpectPageChecksumMismatch(FileDiskManager& manager) {
    std::array<char, PAGE_DATA_SIZE> page{};
    try {
        manager.ReadPage(1, page.data());
        FAIL() << "Corrupted page was accepted";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::PAGE_CHECKSUM_MISMATCH);
        EXPECT_EQ(error.PageId(), 1);
    }
}

}  // namespace

TEST(Crc32cTest, StandardVectorAndHardwareCrossCheck) {
    constexpr char input[] = "123456789";
    const auto* bytes = reinterpret_cast<const uint8_t*>(input);
    EXPECT_EQ(Crc32c(bytes, 9), 0xE3069283U);
    EXPECT_EQ(Crc32cSoftware(bytes, 9), 0xE3069283U);

    constexpr uint32_t seed = 20261030;
    std::mt19937 random(seed);
    std::uniform_int_distribution<int> byte_distribution(0, 255);
    std::vector<uint8_t> buffer(16384);
    for (size_t sample = 0; sample < 256; ++sample) {
        for (auto& byte : buffer) {
            byte = static_cast<uint8_t>(byte_distribution(random));
        }
        const size_t length = sample * 61U % (buffer.size() + 1U);
        EXPECT_EQ(Crc32c(buffer.data(), length), Crc32cSoftware(buffer.data(), length))
            << "seed=" << seed << " sample=" << sample;
    }
}

TEST(DiskManagerTest, RoundTripReopenAndAllZeroPage) {
    TemporaryDatabase database;
    std::array<char, PAGE_DATA_SIZE> written{};
    for (size_t index = 0; index < written.size(); ++index) {
        written[index] = static_cast<char>((index * 37U + 19U) & 0xFFU);
    }
    {
        FileDiskManager manager(database.String());
        EXPECT_EQ(manager.SizeInPages(), 1U);
        manager.Extend(2);
        std::array<char, PAGE_DATA_SIZE> empty{};
        manager.ReadPage(2, empty.data());
        EXPECT_EQ(empty, (std::array<char, PAGE_DATA_SIZE>{}));
        manager.WritePage(1, written.data());
        manager.Sync();
        EXPECT_EQ(manager.SizeInPages(), 3U);
    }

    FileDiskManager reopened(database.String());
    std::array<char, PAGE_DATA_SIZE> read{};
    reopened.ReadPage(1, read.data());
    EXPECT_EQ(read, written);
    EXPECT_EQ(reopened.SizeInPages(), 3U);
}

TEST(DiskManagerTest, RejectsInvalidMagicVersionAndPageSize) {
    {
        TemporaryDatabase database;
        {
            FileDiskManager manager(database.String());
            manager.Sync();
        }
        RewriteByte(database.String(), 16, 'X');
        try {
            FileDiskManager reopened(database.String());
            FAIL() << "Invalid magic was accepted";
        } catch (const DbError& error) {
            EXPECT_EQ(error.Code(), ErrorCode::INVALID_DB_FILE);
        }
    }
    {
        TemporaryDatabase database;
        {
            FileDiskManager manager(database.String());
        }
        RewriteByte(database.String(), 24, 3);
        try {
            FileDiskManager reopened(database.String());
            FAIL() << "Unsupported file version was accepted";
        } catch (const DbError& error) {
            EXPECT_EQ(error.Code(), ErrorCode::UNSUPPORTED_VERSION);
        }
    }
    {
        TemporaryDatabase database;
        {
            FileDiskManager manager(database.String());
        }
        RewriteByte(database.String(), 29, 0);
        try {
            FileDiskManager reopened(database.String());
            FAIL() << "Invalid page size was accepted";
        } catch (const DbError& error) {
            EXPECT_EQ(error.Code(), ErrorCode::HEADER_CORRUPT);
        }
    }
    {
        TemporaryDatabase database;
        {
            FileDiskManager manager(database.String());
        }
        RewriteByte(database.String(), 64, 1);
        try {
            FileDiskManager reopened(database.String());
            FAIL() << "Corrupt header checksum was accepted";
        } catch (const DbError& error) {
            EXPECT_EQ(error.Code(), ErrorCode::HEADER_CORRUPT);
        }
    }
}

TEST(DiskManagerTest, DetectsSingleBitErrorsInSampledOrExhaustivePageBits) {
    TemporaryDatabase database;
    FileDiskManager manager(database.String());
    manager.Extend(1);
    std::array<char, PAGE_DATA_SIZE> body{};
    for (size_t index = 0; index < body.size(); ++index) {
        body[index] = static_cast<char>((index * 13U + 7U) & 0xFFU);
    }
    manager.WritePage(1, body.data());
    const std::vector<uint8_t> original = ReadRawPage(database.String());

    const char* exhaustive_setting = std::getenv("RELDB_EXHAUSTIVE_PAGE_CHECKSUM");
    const bool exhaustive = exhaustive_setting != nullptr && std::string(exhaustive_setting) == "1";
    const size_t bit_count = PAGE_SIZE * 8U;
    constexpr uint32_t seed = 8675309;
    std::vector<size_t> bits;
    if (exhaustive) {
        bits.resize(bit_count);
        for (size_t index = 0; index < bit_count; ++index) {
            bits[index] = index;
        }
    } else {
        std::mt19937 random(seed);
        bits.reserve(512);
        for (size_t index = 0; index < 512; ++index) {
            bits.push_back(static_cast<size_t>(random()) % bit_count);
        }
    }

    for (size_t bit_index : bits) {
        std::vector<uint8_t> corrupted = original;
        corrupted[bit_index / 8U] ^= static_cast<uint8_t>(1U << (bit_index % 8U));
        RewriteRawPage(database.String(), corrupted);
        ExpectPageChecksumMismatch(manager);
    }
    RewriteRawPage(database.String(), original);
    std::cout << "Page checksum bit-flip test seed=" << seed << " tested_bits=" << bits.size()
              << std::endl;
}

TEST(DiskManagerTest, RejectsOutOfRangePagesAndTracksStats) {
    TemporaryDatabase database;
    FileDiskManager manager(database.String());
    std::array<char, PAGE_DATA_SIZE> page{};
    EXPECT_THROW(manager.ReadPage(1, page.data()), DbError);
    EXPECT_THROW(manager.WritePage(1, page.data()), DbError);
    try {
        manager.WritePage(0, page.data());
        FAIL() << "Page zero was overwritten";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::INVALID_ARGUMENT);
    }
    manager.Extend(1);
    manager.ReadPage(1, page.data());
    manager.WritePage(1, page.data());
    const auto before_sync = manager.Stats();
    EXPECT_EQ(before_sync.syncs, 1U);
    manager.Sync();
    const auto after_sync = manager.Stats();
    EXPECT_EQ(after_sync.syncs, before_sync.syncs + 1U);
    EXPECT_GE(after_sync.reads, 1U);
    EXPECT_GE(after_sync.writes, 3U);
    EXPECT_GE(after_sync.bytes, static_cast<uint64_t>(PAGE_SIZE) * 3U);
}

TEST(DiskManagerTest, ConcurrentDistinctPageIo) {
    TemporaryDatabase database;
    FileDiskManager manager(database.String());
    constexpr size_t thread_count = 8;
    constexpr uint32_t seed = 20261030;
    manager.Extend(thread_count);
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&, thread_index]() {
            std::array<char, PAGE_DATA_SIZE> expected{};
            expected.fill(static_cast<char>(thread_index + 1U));
            const relational_db::page_id_t page_id =
                static_cast<relational_db::page_id_t>(thread_index + 1U);
            for (size_t iteration = 0; iteration < 50; ++iteration) {
                manager.WritePage(page_id, expected.data());
                std::array<char, PAGE_DATA_SIZE> actual{};
                manager.ReadPage(page_id, actual.data());
                if (actual != expected) {
                    failed.store(true);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::cout << "Concurrent disk I/O seed=" << seed << std::endl;
    EXPECT_FALSE(failed.load());
}

TEST(DiskManagerTest, ShortReadRaisesAnIoErrorInsteadOfZeroFilling) {
    TemporaryDatabase database;
    FileDiskManager manager(database.String());
    manager.Extend(1);
    std::filesystem::resize_file(database.String(), PAGE_SIZE + 100U);

    std::array<char, PAGE_DATA_SIZE> page{};
    page.fill('x');
    try {
        manager.ReadPage(1, page.data());
        FAIL() << "A truncated page was accepted";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::IO_ERROR);
    }
}

TEST(BufferPoolDiskErrorTest, OutOfRangeFetchDoesNotConsumeAFrame) {
    TemporaryDatabase database;
    auto disk_manager = std::make_shared<FileDiskManager>(database.String());
    relational_db::BufferPoolManager buffer_pool(16, disk_manager);
    EXPECT_THROW(buffer_pool.FetchRead(10), DbError);

    auto page = buffer_pool.NewPageGuarded();
    EXPECT_EQ(page.GetPageId(), 1);
    page.Drop();
    buffer_pool.AssertNoPinsHeld();
}

TEST(FaultInjectingDiskManagerTest, InjectsWriteTornSyncReadAndEnospcFaults) {
    TemporaryDatabase database;
    FileDiskManager base(database.String());
    base.Extend(1);
    FaultInjectingDiskManager fault_manager(base, database.String());
    std::array<char, PAGE_DATA_SIZE> first{};
    first.fill('a');
    std::array<char, PAGE_DATA_SIZE> second{};
    second.fill('b');

    fault_manager.FailNthWrite(1);
    EXPECT_THROW(fault_manager.WritePage(1, first.data()), DbError);
    fault_manager.FailNthWrite(0);
    fault_manager.WritePage(1, first.data());

    fault_manager.TearNextWrite(32);
    EXPECT_THROW(fault_manager.WritePage(1, second.data()), DbError);
    std::array<char, PAGE_DATA_SIZE> actual{};
    try {
        base.ReadPage(1, actual.data());
        FAIL() << "A physically torn page was accepted";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::PAGE_CHECKSUM_MISMATCH);
    }

    const uint64_t sync_count = base.Stats().syncs;
    fault_manager.DropSync();
    fault_manager.Sync();
    EXPECT_EQ(base.Stats().syncs, sync_count);

    base.WritePage(1, first.data());
    fault_manager.FlipReadBit(0, 1);
    fault_manager.ReadPage(1, actual.data());
    EXPECT_EQ(actual[0], static_cast<char>(first[0] ^ 1));

    fault_manager.InjectEnospcOnWrite();
    try {
        fault_manager.WritePage(1, first.data());
        FAIL() << "Injected ENOSPC was ignored";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::IO_ERROR);
        EXPECT_NE(std::string(error.what()).find("ENOSPC"), std::string::npos);
    }
}
