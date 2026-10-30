#ifndef DISK_MANAGER_H
#define DISK_MANAGER_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "config.h"

namespace relational_db {

enum class PageType : uint16_t {
    HEADER = 0,
    FREE = 1,
    BTREE_INTERNAL = 2,
    BTREE_LEAF = 3,
    HEAP = 4,
    CATALOG = 5,
    INDEX_META = 6
};

struct PageHeader {
    uint32_t crc32c;
    uint16_t type;
    uint16_t flags;
    uint64_t lsn;
};

static_assert(sizeof(PageHeader) == PAGE_HEADER_SIZE, "PageHeader must be 16 bytes");
static_assert(offsetof(PageHeader, crc32c) == 0, "PageHeader CRC offset changed");
static_assert(offsetof(PageHeader, type) == 4, "PageHeader type offset changed");
static_assert(offsetof(PageHeader, flags) == 6, "PageHeader flags offset changed");
static_assert(offsetof(PageHeader, lsn) == 8, "PageHeader LSN offset changed");

struct DiskStats {
    static constexpr size_t kLatencyBucketCount = 16;

    uint64_t reads{0};
    uint64_t writes{0};
    uint64_t syncs{0};
    uint64_t bytes{0};
    std::array<uint64_t, kLatencyBucketCount> read_latency_us{};
    std::array<uint64_t, kLatencyBucketCount> write_latency_us{};
    std::array<uint64_t, kLatencyBucketCount> sync_latency_us{};
};

class IDiskManager {
   public:
    virtual ~IDiskManager() = default;

    virtual void ReadPage(page_id_t page_id, char* page_data) = 0;
    virtual void WritePage(page_id_t page_id, const char* page_data, PageType type = PageType::HEAP,
                           uint16_t flags = 0, uint64_t lsn = 0) = 0;
    virtual void WriteHeaderPage(const char* page_data) = 0;
    virtual void Sync() = 0;
    virtual uint64_t SizeInPages() const = 0;
    virtual void Extend(uint64_t page_count) = 0;
    virtual DiskStats Stats() const = 0;
};

class FileDiskManager final : public IDiskManager {
   public:
    explicit FileDiskManager(const std::string& db_file);
    ~FileDiskManager() override;

    FileDiskManager(const FileDiskManager&) = delete;
    FileDiskManager& operator=(const FileDiskManager&) = delete;

    void Open();
    void ReadPage(page_id_t page_id, char* page_data) override;
    void WritePage(page_id_t page_id, const char* page_data, PageType type = PageType::HEAP,
                   uint16_t flags = 0, uint64_t lsn = 0) override;
    void WriteHeaderPage(const char* page_data) override;
    void Sync() override;
    uint64_t SizeInPages() const override;
    void Extend(uint64_t page_count) override;
    DiskStats Stats() const override;

   private:
    void InitializeNewFile();
    void ValidateExistingFile();
    void WriteDatabaseHeader(uint64_t page_count);
    void GrowPhysicalFile(uint64_t logical_page_count);
    void ReadExact(uint64_t offset, char* buffer, size_t length, page_id_t page_id);
    void WriteExact(uint64_t offset, const char* buffer, size_t length);
    void RecordLatency(std::array<std::atomic<uint64_t>, DiskStats::kLatencyBucketCount>& buckets,
                       uint64_t elapsed_us);

    std::string file_name_;
    int file_descriptor_{-1};
    std::atomic<uint64_t> page_count_{0};
    mutable std::mutex open_mutex_;
    mutable std::mutex header_mutex_;
    std::atomic<bool> opened_{false};
    std::atomic<uint64_t> reads_{0};
    std::atomic<uint64_t> writes_{0};
    std::atomic<uint64_t> syncs_{0};
    std::atomic<uint64_t> bytes_{0};
    std::array<std::atomic<uint64_t>, DiskStats::kLatencyBucketCount> read_latency_us_{};
    std::array<std::atomic<uint64_t>, DiskStats::kLatencyBucketCount> write_latency_us_{};
    std::array<std::atomic<uint64_t>, DiskStats::kLatencyBucketCount> sync_latency_us_{};
};

using DiskManager = FileDiskManager;

}  // namespace relational_db

#endif
