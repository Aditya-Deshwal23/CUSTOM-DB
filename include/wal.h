#ifndef WAL_H
#define WAL_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "disk_manager.h"

namespace relational_db {

class BufferPoolManager;
class WriteAheadLog;

enum class LogRecordType : uint8_t { PAGE_IMAGE = 1, COMMIT = 2, CHECKPOINT = 3 };
enum class DurabilityMode { FULL, NORMAL, OFF };

struct PageBeforeImage {
    page_id_t page_id{INVALID_PAGE_ID};
    PageType type{PageType::HEAP};
    uint16_t flags{0};
    uint64_t lsn{0};
    bool dirty{false};
    std::array<char, PAGE_DATA_SIZE> body{};
};

struct WalRecoveryStats {
    uint64_t records{0};
    uint64_t commits{0};
    uint64_t pages_replayed{0};
    uint64_t truncated_bytes{0};
    uint64_t elapsed_ms{0};
};

class TxnContext {
   public:
    TxnContext(BufferPoolManager& buffer_pool, WriteAheadLog& wal, uint64_t txn_id,
               uint64_t page_count_before);
    TxnContext(const TxnContext&) = delete;
    TxnContext& operator=(const TxnContext&) = delete;
    ~TxnContext();

    void Commit();
    void Rollback() noexcept;
    uint64_t Id() const noexcept {
        return txn_id_;
    }

   private:
    friend class BufferPoolManager;
    friend class WriteAheadLog;
    friend class TransactionAwareWriteBackPolicy;

    BufferPoolManager* buffer_pool_;
    WriteAheadLog* wal_;
    uint64_t txn_id_;
    uint64_t page_count_before_;
    bool completed_{false};
    std::unordered_map<page_id_t, PageBeforeImage> before_images_;
    std::unordered_map<page_id_t, bool> free_page_state_before_;
};

class WriteAheadLog {
   public:
    struct CommitPage {
        page_id_t page_id;
        PageType type;
        uint16_t flags;
        std::array<char, PAGE_DATA_SIZE> body;
    };

    explicit WriteAheadLog(const std::string& database_path);
    WriteAheadLog(const std::string& database_path, DurabilityMode mode,
                  uint64_t sync_interval_ms = 1000, uint64_t max_bytes = 16U * 1024U * 1024U);
    ~WriteAheadLog();

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    WalRecoveryStats Recover(IDiskManager& disk_manager);
    void Checkpoint(BufferPoolManager& buffer_pool);
    uint64_t GetNextLSN() const noexcept {
        return next_lsn_;
    }
    uint64_t GetNextTxnId() const noexcept {
        return next_txn_id_;
    }
    uint64_t GetFirstUnsyncedLSN() const;
    uint64_t GetLogCount() const noexcept {
        return log_count_;
    }
    uint64_t SizeBytes() const;
    DurabilityMode Mode() const noexcept {
        return mode_;
    }
    bool ShouldCheckpoint() const;
    void EnsureDurable(uint64_t page_lsn);

   private:
    friend class BufferPoolManager;
    friend class TxnContext;

    std::vector<uint64_t> Commit(uint64_t txn_id, const std::vector<CommitPage>& pages);
    void Sync();
    void RewriteHeader(uint64_t base_lsn);
    void EnsureRecovered() const;
    void ThrowSyncError() const;
    void RunSyncTimer();

    std::string database_path_;
    std::string filename_;
    int file_descriptor_{-1};
    DurabilityMode mode_{DurabilityMode::FULL};
    uint64_t sync_interval_ms_{1000};
    uint64_t max_bytes_{16U * 1024U * 1024U};
    uint64_t next_lsn_{0};
    uint64_t durable_lsn_{0};
    uint64_t next_txn_id_{1};
    uint64_t log_count_{0};
    uint64_t file_size_{0};
    std::chrono::steady_clock::time_point last_sync_;
    bool recovered_{false};
    bool stop_sync_thread_{false};
    std::string sync_error_;
    std::condition_variable sync_condition_;
    std::thread sync_thread_;
    mutable std::mutex latch_;
};

class FailPoint {
   public:
    static void Hit(const std::string& name);
    static bool IsConfigured(const std::string& name, const std::string& action);
};

}  // namespace relational_db

#endif
