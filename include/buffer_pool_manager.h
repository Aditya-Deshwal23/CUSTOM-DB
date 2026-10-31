#ifndef BUFFER_POOL_MANAGER_H
#define BUFFER_POOL_MANAGER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clock_replacer.h"
#include "concurrency.h"
#include "config.h"
#include "disk_manager.h"
#include "page.h"
#include "page_guard.h"
#include "wal.h"

namespace relational_db {

class PageAllocator;

class WriteBackPolicy {
   public:
    virtual ~WriteBackPolicy() = default;
    virtual bool CanWriteBack(page_id_t page_id) const = 0;
    virtual void BeforeWriteBack(page_id_t page_id, uint64_t lsn) = 0;
};

class AllowAllWriteBackPolicy final : public WriteBackPolicy {
   public:
    bool CanWriteBack(page_id_t) const override {
        return true;
    }
    void BeforeWriteBack(page_id_t, uint64_t) override {}
};

struct BufferPoolStats {
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
    uint64_t dirty_write_backs{0};
    uint64_t waits{0};
};

struct FrameInfo {
    frame_id_t frame_id{0};
    page_id_t page_id{INVALID_PAGE_ID};
    int pin_count{0};
    bool dirty{false};
    bool ref_bit{false};
    size_t clock_hand{0};
};

class BufferPoolManager {
    friend class TxnContext;
    friend class WriteAheadLog;
    friend class TransactionAwareWriteBackPolicy;

   public:
    explicit BufferPoolManager(std::shared_ptr<IDiskManager> disk_manager);
    BufferPoolManager(size_t pool_size, std::shared_ptr<IDiskManager> disk_manager);
    ~BufferPoolManager();

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    ReadPageGuard FetchRead(page_id_t page_id);
    WritePageGuard FetchWrite(page_id_t page_id);
    BasicPageGuard FetchBasic(page_id_t page_id);
    WritePageGuard NewPageGuarded();
    std::shared_ptr<TxnContext> BeginTransaction(WriteAheadLog& wal);

    bool DeletePage(page_id_t page_id);
    bool FlushPage(page_id_t page_id);
    void FlushAll();

    void SetWriteBackPolicy(std::shared_ptr<WriteBackPolicy> policy);
    BufferPoolStats SnapshotStats() const;
    std::vector<FrameInfo> Snapshot() const;
    size_t pinned_frames() const;
    void AssertNoPinsHeld() const;
    size_t PoolSize() const noexcept {
        return frames_.size();
    }
    uint64_t PageCount() const {
        return disk_manager_->SizeInPages();
    }
    void VerifyDiskPages() const;

    uint64_t GetHits() const {
        return hits_.load(std::memory_order_relaxed);
    }
    uint64_t GetMisses() const {
        return misses_.load(std::memory_order_relaxed);
    }

   private:
    friend class PageAllocator;

    PageGuard Fetch(page_id_t page_id, GuardMode mode, bool allow_free_page = false);
    PageGuard AllocatePageFrame(page_id_t page_id);
    WritePageGuard AllocateFreePageGuarded(page_id_t page_id);
    bool RetirePage(page_id_t page_id);
    static void ReleaseGuard(void* owner, Frame* frame, bool dirty,
                             std::thread::id origin_thread) noexcept;
    void ReleaseFrame(Frame* frame, bool dirty, std::thread::id origin_thread) noexcept;
    void RegisterPinLocked(std::thread::id origin_thread);
    static void RegisterMutation(void* owner, Frame* frame);
    void RegisterMutation(Frame* frame);
    void CommitTransaction(TxnContext& transaction);
    void RollbackTransaction(TxnContext& transaction) noexcept;
    void BeginCheckpoint(WriteAheadLog& wal);
    void EndCheckpoint() noexcept;
    void RecordFreePageState(page_id_t page_id, bool will_be_free);
    bool HasActiveTransaction() const;
    void FlushFrame(Frame* frame);
    static size_t ConfiguredPoolSize();

    std::vector<Frame> frames_;
    std::shared_ptr<IDiskManager> disk_manager_;
    std::unique_ptr<Replacer> replacer_;
    std::unique_ptr<PageAllocator> page_allocator_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::unordered_map<page_id_t, frame_id_t> in_flight_write_backs_;
    std::unordered_set<page_id_t> free_pages_;
    std::unordered_map<std::thread::id, uint64_t> thread_pins_;
    std::vector<frame_id_t> free_list_;
    mutable RankedMutex pool_mutex_{LockRank::BUFFER_POOL};
    std::shared_ptr<WriteBackPolicy> write_back_policy_;
    std::shared_ptr<TxnContext> active_transaction_;
    std::shared_ptr<TxnContext> active_transaction_snapshot_;
    WriteAheadLog* wal_{nullptr};
    bool checkpoint_in_progress_{false};

    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> evictions_{0};
    std::atomic<uint64_t> dirty_write_backs_{0};
    std::atomic<uint64_t> waits_{0};
};

}  // namespace relational_db

#endif
