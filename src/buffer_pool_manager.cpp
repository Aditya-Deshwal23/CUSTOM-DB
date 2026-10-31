#include "buffer_pool_manager.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

#include "db_error.h"
#include "logging.h"
#include "page_allocator.h"

namespace relational_db {
class TransactionAwareWriteBackPolicy final : public WriteBackPolicy {
   public:
    TransactionAwareWriteBackPolicy(BufferPoolManager& buffer_pool,
                                    std::shared_ptr<WriteBackPolicy> delegate)
        : buffer_pool_(buffer_pool), delegate_(std::move(delegate)) {}

    bool CanWriteBack(page_id_t page_id) const override {
        if (!delegate_->CanWriteBack(page_id)) {
            return false;
        }
        std::lock_guard<RankedMutex> lock(buffer_pool_.pool_mutex_);
        const std::shared_ptr<TxnContext> transaction = buffer_pool_.active_transaction_;
        if (!transaction) {
            return true;
        }
        std::lock_guard<RankedMutex> transaction_lock(transaction->state_mutex_);
        return transaction->before_images_.find(page_id) == transaction->before_images_.end();
    }

    void BeforeWriteBack(page_id_t page_id, uint64_t lsn) override {
        delegate_->BeforeWriteBack(page_id, lsn);
    }

   private:
    BufferPoolManager& buffer_pool_;
    std::shared_ptr<WriteBackPolicy> delegate_;
};

namespace {

size_t ValidatePoolSize(size_t pool_size) {
    if (pool_size == 0 || pool_size > static_cast<size_t>(std::numeric_limits<frame_id_t>::max())) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Buffer-pool frame count is out of range");
    }
    return pool_size;
}

}  // namespace

BufferPoolManager::BufferPoolManager(std::shared_ptr<IDiskManager> disk_manager)
    : BufferPoolManager(4096, std::move(disk_manager), "lruk") {}

BufferPoolManager::BufferPoolManager(size_t pool_size, std::shared_ptr<IDiskManager> disk_manager)
    : BufferPoolManager(pool_size, std::move(disk_manager), "lruk") {}

BufferPoolManager::BufferPoolManager(size_t pool_size, std::shared_ptr<IDiskManager> disk_manager,
                                     const std::string& replacement_policy)
    : frames_(ValidatePoolSize(pool_size)),
      disk_manager_(std::move(disk_manager)),
      write_back_policy_(std::make_shared<TransactionAwareWriteBackPolicy>(
          *this, std::make_shared<AllowAllWriteBackPolicy>())) {
    if (!disk_manager_) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Buffer pool requires a disk manager");
    }

    if (replacement_policy == "clock") {
        replacer_ = std::make_unique<ClockReplacer>(pool_size);
    } else if (replacement_policy == "lruk") {
        replacer_ = std::make_unique<LruKReplacer>(pool_size, 2);
    } else {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "BUFFER_POOL_POLICY must be either 'clock' or 'lruk'");
    }

    free_list_.reserve(pool_size);
    in_flight_write_backs_.reserve(pool_size);
    free_pages_.reserve(pool_size);
    for (size_t index = pool_size; index > 0; --index) {
        free_list_.push_back(static_cast<frame_id_t>(index - 1U));
    }
    page_allocator_ = std::make_unique<PageAllocator>(*this);
    page_allocator_->LoadFreeList();
}

BufferPoolManager::~BufferPoolManager() {
    try {
        FlushAll();
    } catch (const std::exception& error) {
        LogMessage("error", "buffer_pool_shutdown_flush_failed", error.what());
    }
}

ReadPageGuard BufferPoolManager::FetchRead(page_id_t page_id) {
    return Fetch(page_id, GuardMode::READ);
}

WritePageGuard BufferPoolManager::FetchWrite(page_id_t page_id) {
    return Fetch(page_id, GuardMode::WRITE);
}

BasicPageGuard BufferPoolManager::FetchBasic(page_id_t page_id) {
    return Fetch(page_id, GuardMode::BASIC);
}

WritePageGuard BufferPoolManager::NewPageGuarded() {
    return page_allocator_->NewPage();
}

std::shared_ptr<TxnContext> BufferPoolManager::BeginTransaction(WriteAheadLog& wal) {
    wal.EnsureRecovered();
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    if (checkpoint_in_progress_) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Cannot begin a write transaction during a checkpoint");
    }
    if (active_transaction_) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Buffer pool already has an active write transaction");
    }
    if (wal_ != nullptr && wal_ != &wal) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Buffer pool cannot switch WALs after writes have started");
    }
    wal_ = &wal;
    active_transaction_ =
        std::make_shared<TxnContext>(*this, wal, wal.GetNextTxnId(), disk_manager_->SizeInPages());
    std::atomic_store(&active_transaction_snapshot_, active_transaction_);
    return active_transaction_;
}

void BufferPoolManager::RegisterMutation(void* owner, Frame* frame) {
    static_cast<BufferPoolManager*>(owner)->RegisterMutation(frame);
}

void BufferPoolManager::RegisterMutation(Frame* frame) {
    const std::shared_ptr<TxnContext> transaction = std::atomic_load(&active_transaction_snapshot_);
    if (!transaction) {
        return;
    }
    std::lock_guard<RankedMutex> lock(transaction->state_mutex_);
    if (transaction->before_images_.find(frame->page_id) != transaction->before_images_.end()) {
        return;
    }
    PageBeforeImage before{};
    before.page_id = frame->page_id;
    before.type = frame->page_type;
    before.flags = frame->page_flags;
    before.lsn = frame->lsn;
    before.dirty = frame->dirty.load(std::memory_order_acquire);
    before.body = frame->data;
    transaction->before_images_.emplace(frame->page_id, std::move(before));
}

bool BufferPoolManager::HasActiveTransaction() const {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    return static_cast<bool>(active_transaction_);
}

void BufferPoolManager::BeginCheckpoint(WriteAheadLog& wal) {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    if (checkpoint_in_progress_ || active_transaction_) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Cannot checkpoint while another checkpoint or write transaction is active");
    }
    if (wal_ != nullptr && wal_ != &wal) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Checkpoint WAL does not match the buffer-pool WAL");
    }
    wal_ = &wal;
    checkpoint_in_progress_ = true;
}

void BufferPoolManager::EndCheckpoint() noexcept {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    checkpoint_in_progress_ = false;
}

void BufferPoolManager::RecordFreePageState(page_id_t page_id, bool will_be_free) {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    if (!active_transaction_) {
        return;
    }
    std::lock_guard<RankedMutex> transaction_lock(active_transaction_->state_mutex_);
    if (active_transaction_->free_page_state_before_.find(page_id) ==
        active_transaction_->free_page_state_before_.end()) {
        active_transaction_->free_page_state_before_.emplace(
            page_id, free_pages_.find(page_id) != free_pages_.end());
    }
    if (will_be_free) {
        free_pages_.insert(page_id);
    } else {
        free_pages_.erase(page_id);
    }
}

void BufferPoolManager::CommitTransaction(TxnContext& transaction) {
    std::vector<WriteAheadLog::CommitPage> pages;
    std::vector<std::pair<Frame*, uint64_t>> stamped_frames;
    std::unordered_map<page_id_t, PageBeforeImage> before_images;
    {
        std::lock_guard<RankedMutex> transaction_lock(transaction.state_mutex_);
        before_images = transaction.before_images_;
    }
    pages.reserve(before_images.size());
    stamped_frames.reserve(before_images.size());

    for (const auto& entry : before_images) {
        Frame* frame = nullptr;
        {
            std::lock_guard<RankedMutex> lock(pool_mutex_);
            const auto mapping = page_table_.find(entry.first);
            if (mapping == page_table_.end()) {
                throw DbError(ErrorCode::PAGE_CORRUPT,
                              "A transaction page left the buffer pool before commit", entry.first);
            }
            frame = &frames_[static_cast<size_t>(mapping->second)];
        }
        std::unique_lock<RankedSharedMutex> page_lock(frame->rwlatch);
        WriteAheadLog::CommitPage image{};
        image.page_id = entry.first;
        image.type = frame->page_type;
        image.flags = frame->page_flags;
        image.body = frame->data;
        pages.push_back(std::move(image));
        stamped_frames.emplace_back(frame, 0);
    }

    const std::vector<uint64_t> lsns = pages.empty()
                                           ? std::vector<uint64_t>{}
                                           : transaction.wal_->Commit(transaction.txn_id_, pages);
    for (size_t index = 0; index < stamped_frames.size(); ++index) {
        std::unique_lock<RankedSharedMutex> page_lock(stamped_frames[index].first->rwlatch);
        stamped_frames[index].first->lsn = lsns[index];
        stamped_frames[index].first->dirty.store(true, std::memory_order_release);
    }
    {
        std::lock_guard<RankedMutex> lock(pool_mutex_);
        if (active_transaction_.get() != &transaction) {
            throw DbError(ErrorCode::PAGE_CORRUPT,
                          "Buffer-pool transaction ownership changed during commit");
        }
        active_transaction_.reset();
        std::atomic_store(&active_transaction_snapshot_, std::shared_ptr<TxnContext>{});
    }
}

void BufferPoolManager::RollbackTransaction(TxnContext& transaction) noexcept {
    std::unordered_map<page_id_t, PageBeforeImage> before_images;
    std::unordered_map<page_id_t, bool> free_page_state_before;
    {
        std::lock_guard<RankedMutex> transaction_lock(transaction.state_mutex_);
        before_images = transaction.before_images_;
        free_page_state_before = transaction.free_page_state_before_;
    }
    for (const auto& entry : before_images) {
        Frame* frame = nullptr;
        {
            std::lock_guard<RankedMutex> lock(pool_mutex_);
            const auto mapping = page_table_.find(entry.first);
            if (mapping != page_table_.end()) {
                frame = &frames_[static_cast<size_t>(mapping->second)];
            }
        }
        if (frame == nullptr) {
            continue;
        }
        std::unique_lock<RankedSharedMutex> page_lock(frame->rwlatch);
        frame->data = entry.second.body;
        frame->page_type = entry.second.type;
        frame->page_flags = entry.second.flags;
        frame->lsn = entry.second.lsn;
        frame->dirty.store(entry.second.dirty, std::memory_order_release);
    }
    {
        std::lock_guard<RankedMutex> lock(pool_mutex_);
        for (const auto& entry : free_page_state_before) {
            if (entry.second) {
                free_pages_.insert(entry.first);
            } else {
                free_pages_.erase(entry.first);
            }
        }
        if (active_transaction_.get() == &transaction) {
            active_transaction_.reset();
            std::atomic_store(&active_transaction_snapshot_, std::shared_ptr<TxnContext>{});
        }
    }
    disk_manager_->RestorePageCount(transaction.page_count_before_);
}

PageGuard BufferPoolManager::Fetch(page_id_t page_id, GuardMode mode, bool allow_free_page) {
    if (page_id < 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Page ID cannot be negative", page_id);
    }
    if (static_cast<uint64_t>(page_id) >= disk_manager_->SizeInPages()) {
        throw DbError(ErrorCode::PAGE_OUT_OF_RANGE, "Page ID is outside the database file",
                      page_id);
    }

    bool recorded_miss = false;
    for (;;) {
        Frame* frame = nullptr;
        frame_id_t frame_id = -1;
        page_id_t evicted_page_id = INVALID_PAGE_ID;
        bool evicted_dirty = false;
        uint64_t evicted_lsn = 0;
        PageType evicted_type = PageType::HEAP;
        uint16_t evicted_flags = 0;
        std::array<char, PAGE_DATA_SIZE> evicted_data{};
        bool claimed_free_page = false;
        std::shared_ptr<WriteBackPolicy> policy;
        WriteAheadLog* wal = nullptr;
        {
            std::unique_lock<RankedMutex> lock(pool_mutex_);
            const auto write_back = in_flight_write_backs_.find(page_id);
            if (write_back != in_flight_write_backs_.end()) {
                const frame_id_t pending_frame_id = write_back->second;
                Frame& pending = frames_[static_cast<size_t>(pending_frame_id)];
                waits_.fetch_add(1, std::memory_order_relaxed);
                pending.condition.wait(lock, [&]() {
                    const auto current = in_flight_write_backs_.find(page_id);
                    return current == in_flight_write_backs_.end() ||
                           current->second != pending_frame_id;
                });
                continue;
            }
            const auto free_page = free_pages_.find(page_id);
            if (free_page != free_pages_.end()) {
                if (!allow_free_page) {
                    throw DbError(ErrorCode::PAGE_OUT_OF_RANGE,
                                  "Page ID has been deleted and is on the free list", page_id);
                }
                if (active_transaction_) {
                    std::lock_guard<RankedMutex> transaction_lock(
                        active_transaction_->state_mutex_);
                    active_transaction_->free_page_state_before_.emplace(page_id, true);
                }
                free_pages_.erase(free_page);
                claimed_free_page = true;
            }
            const auto existing = page_table_.find(page_id);
            if (existing != page_table_.end()) {
                frame_id = existing->second;
                frame = &frames_[static_cast<size_t>(frame_id)];
                if (frame->io_state != IoState::READY) {
                    waits_.fetch_add(1, std::memory_order_relaxed);
                    frame->condition.wait(lock, [&]() {
                        const auto mapping = page_table_.find(page_id);
                        return mapping == page_table_.end() || mapping->second != frame_id ||
                               frames_[static_cast<size_t>(mapping->second)].io_state ==
                                   IoState::READY;
                    });
                    continue;
                }
                if (frame->page_id != page_id) {
                    throw DbError(ErrorCode::IO_ERROR, "Buffer-pool page-table invariant failed",
                                  page_id);
                }
                frame->pin_count.fetch_add(1, std::memory_order_relaxed);
                replacer_->Pin(frame_id);
                RegisterPinLocked(std::this_thread::get_id());
                hits_.fetch_add(1, std::memory_order_relaxed);
                lock.unlock();
                if (mode == GuardMode::READ) {
                    frame->rwlatch.lock_shared();
                } else if (mode == GuardMode::WRITE) {
                    frame->rwlatch.lock();
                    RegisterMutation(frame);
                }
                return PageGuard(frame, frame_id, this, &BufferPoolManager::ReleaseGuard, mode,
                                 &BufferPoolManager::RegisterMutation);
            }

            if (!recorded_miss) {
                misses_.fetch_add(1, std::memory_order_relaxed);
                recorded_miss = true;
            }
            const bool took_free_frame = !free_list_.empty();
            if (took_free_frame) {
                frame_id = free_list_.back();
                free_list_.pop_back();
            } else if (!replacer_->Victim(&frame_id)) {
                if (claimed_free_page) {
                    free_pages_.insert(page_id);
                }
                if (active_transaction_) {
                    throw DbError(ErrorCode::TXN_TOO_LARGE,
                                  "Active statement cannot reserve another buffer-pool frame",
                                  page_id);
                }
                throw DbError(ErrorCode::BUFFER_POOL_EXHAUSTED,
                              "All buffer-pool frames are pinned or unavailable", page_id);
            }
            Frame* candidate = &frames_[static_cast<size_t>(frame_id)];
            bool candidate_protected = false;
            if (!took_free_frame && candidate->page_id != INVALID_PAGE_ID && active_transaction_) {
                std::lock_guard<RankedMutex> transaction_lock(active_transaction_->state_mutex_);
                candidate_protected =
                    active_transaction_->before_images_.find(candidate->page_id) !=
                    active_transaction_->before_images_.end();
            }
            if (candidate_protected) {
                std::vector<frame_id_t> protected_frames{frame_id};
                frame_id = -1;
                while (protected_frames.size() <= frames_.size()) {
                    frame_id_t next_candidate_id = -1;
                    if (!replacer_->Victim(&next_candidate_id)) {
                        break;
                    }
                    Frame& next_candidate = frames_[static_cast<size_t>(next_candidate_id)];
                    bool protected_by_transaction = false;
                    if (next_candidate.page_id != INVALID_PAGE_ID && active_transaction_) {
                        std::lock_guard<RankedMutex> transaction_lock(
                            active_transaction_->state_mutex_);
                        protected_by_transaction =
                            active_transaction_->before_images_.find(next_candidate.page_id) !=
                            active_transaction_->before_images_.end();
                    }
                    if (next_candidate.page_id == INVALID_PAGE_ID || !protected_by_transaction) {
                        frame_id = next_candidate_id;
                        frame = &next_candidate;
                        break;
                    }
                    protected_frames.push_back(next_candidate_id);
                }
                for (const frame_id_t protected_frame : protected_frames) {
                    replacer_->Unpin(protected_frame);
                }
                if (frame_id < 0) {
                    if (claimed_free_page) {
                        free_pages_.insert(page_id);
                    }
                    throw DbError(ErrorCode::TXN_TOO_LARGE,
                                  "Active statement exceeds the buffer-pool capacity", page_id);
                }
            }
            frame = &frames_[static_cast<size_t>(frame_id)];
            evicted_page_id = frame->page_id;
            evicted_dirty =
                evicted_page_id != INVALID_PAGE_ID && frame->dirty.load(std::memory_order_acquire);
            evicted_lsn = frame->lsn;
            evicted_type = frame->page_type;
            evicted_flags = frame->page_flags;
            if (evicted_page_id != INVALID_PAGE_ID) {
                std::copy(frame->data.begin(), frame->data.end(), evicted_data.begin());
                page_table_.erase(evicted_page_id);
                if (evicted_dirty) {
                    in_flight_write_backs_[evicted_page_id] = frame_id;
                }
            }
            frame->page_id = page_id;
            frame->pin_count.store(1, std::memory_order_release);
            frame->io_state = evicted_dirty ? IoState::WRITING_BACK : IoState::LOADING;
            frame->page_flags = 0;
            if (!evicted_dirty) {
                frame->dirty.store(false, std::memory_order_release);
            }
            page_table_[page_id] = frame_id;
            policy = write_back_policy_;
            wal = wal_;
        }

        bool latch_acquired = false;
        bool write_back_completed = false;
        try {
            if (evicted_dirty) {
                if (!policy->CanWriteBack(evicted_page_id)) {
                    throw DbError(ErrorCode::BUFFER_POOL_EXHAUSTED,
                                  "Write-back policy prevents evicting dirty page",
                                  evicted_page_id);
                }
                policy->BeforeWriteBack(evicted_page_id, evicted_lsn);
                if (wal != nullptr) {
                    wal->EnsureDurable(evicted_lsn);
                }
                if (evicted_page_id == 0) {
                    disk_manager_->WriteHeaderPage(frame->data.data(), evicted_flags, evicted_lsn);
                } else {
                    disk_manager_->WritePage(evicted_page_id, frame->data.data(), evicted_type,
                                             evicted_flags, evicted_lsn);
                }
                write_back_completed = true;
                dirty_write_backs_.fetch_add(1, std::memory_order_relaxed);
                frame->dirty.store(false, std::memory_order_release);
                std::lock_guard<RankedMutex> lock(pool_mutex_);
                frame->io_state = IoState::LOADING;
            }
            PageType loaded_type = PageType::HEAP;
            uint16_t loaded_flags = 0;
            uint64_t loaded_lsn = 0;
            disk_manager_->ReadPage(page_id, frame->data.data(), loaded_type, loaded_flags,
                                    loaded_lsn);
            frame->page_type = loaded_type;
            frame->page_flags = loaded_flags;
            frame->lsn = loaded_lsn;
            {
                std::lock_guard<RankedMutex> lock(pool_mutex_);
                if (mode == GuardMode::READ) {
                    frame->rwlatch.lock_shared();
                    latch_acquired = true;
                } else if (mode == GuardMode::WRITE) {
                    frame->rwlatch.lock();
                    latch_acquired = true;
                    RegisterMutation(frame);
                }
                if (evicted_dirty) {
                    in_flight_write_backs_.erase(evicted_page_id);
                }
                frame->io_state = IoState::READY;
                RegisterPinLocked(std::this_thread::get_id());
            }
            if (evicted_page_id != INVALID_PAGE_ID) {
                evictions_.fetch_add(1, std::memory_order_relaxed);
            }
            frame->condition.notify_all();
            return PageGuard(frame, frame_id, this, &BufferPoolManager::ReleaseGuard, mode,
                             &BufferPoolManager::RegisterMutation);
        } catch (...) {
            if (latch_acquired && mode == GuardMode::READ) {
                frame->rwlatch.unlock_shared();
            } else if (latch_acquired && mode == GuardMode::WRITE) {
                frame->rwlatch.unlock();
            }
            std::lock_guard<RankedMutex> lock(pool_mutex_);
            const auto mapping = page_table_.find(page_id);
            if (mapping != page_table_.end() && mapping->second == frame_id) {
                page_table_.erase(mapping);
            }
            if (evicted_dirty) {
                in_flight_write_backs_.erase(evicted_page_id);
            }
            frame->pin_count.store(0, std::memory_order_release);
            frame->page_id = evicted_page_id;
            frame->page_type = evicted_type;
            frame->page_flags = evicted_flags;
            frame->io_state = IoState::READY;
            if (evicted_page_id == INVALID_PAGE_ID) {
                frame->dirty.store(false, std::memory_order_release);
                free_list_.push_back(frame_id);
            } else {
                std::copy(evicted_data.begin(), evicted_data.end(), frame->data.begin());
                frame->lsn = evicted_lsn;
                frame->dirty.store(evicted_dirty && !write_back_completed,
                                   std::memory_order_release);
                page_table_[evicted_page_id] = frame_id;
                replacer_->Unpin(frame_id);
            }
            if (claimed_free_page) {
                free_pages_.insert(page_id);
            }
            frame->condition.notify_all();
            throw;
        }
    }
}

PageGuard BufferPoolManager::AllocatePageFrame(page_id_t page_id) {
    return Fetch(page_id, GuardMode::WRITE);
}

WritePageGuard BufferPoolManager::AllocateFreePageGuarded(page_id_t page_id) {
    return Fetch(page_id, GuardMode::WRITE, true);
}

bool BufferPoolManager::RetirePage(page_id_t page_id) {
    Frame* frame = nullptr;
    frame_id_t frame_id = -1;
    uint64_t lsn = 0;
    std::shared_ptr<WriteBackPolicy> policy;
    WriteAheadLog* wal = nullptr;
    {
        std::lock_guard<RankedMutex> lock(pool_mutex_);
        const auto mapping = page_table_.find(page_id);
        if (mapping == page_table_.end()) {
            return false;
        }
        frame_id = mapping->second;
        frame = &frames_[static_cast<size_t>(frame_id)];
        if (frame->pin_count.load(std::memory_order_acquire) != 0 ||
            frame->io_state != IoState::READY) {
            return false;
        }
        frame->pin_count.store(1, std::memory_order_release);
        replacer_->Pin(frame_id);
        free_pages_.insert(page_id);
        frame->io_state = IoState::WRITING_BACK;
        lsn = frame->lsn;
        policy = write_back_policy_;
        wal = wal_;
    }

    std::unique_lock<RankedSharedMutex> page_lock(frame->rwlatch, std::defer_lock);
    try {
        if (!policy->CanWriteBack(page_id)) {
            std::lock_guard<RankedMutex> lock(pool_mutex_);
            free_pages_.erase(page_id);
            frame->io_state = IoState::READY;
            frame->pin_count.store(0, std::memory_order_release);
            replacer_->Unpin(frame_id);
            frame->condition.notify_all();
            return false;
        }
        policy->BeforeWriteBack(page_id, lsn);
        page_lock.lock();
        const PageType type = frame->page_type;
        const uint16_t flags = frame->page_flags;
        if (wal != nullptr) {
            wal->EnsureDurable(lsn);
        }
        disk_manager_->WritePage(page_id, frame->data.data(), type, flags, lsn);
        page_lock.unlock();
    } catch (...) {
        if (page_lock.owns_lock()) {
            page_lock.unlock();
        }
        std::lock_guard<RankedMutex> lock(pool_mutex_);
        free_pages_.erase(page_id);
        frame->io_state = IoState::READY;
        frame->pin_count.store(0, std::memory_order_release);
        replacer_->Unpin(frame_id);
        frame->condition.notify_all();
        throw;
    }

    {
        std::lock_guard<RankedMutex> lock(pool_mutex_);
        page_table_.erase(page_id);
        frame->page_id = INVALID_PAGE_ID;
        frame->dirty.store(false, std::memory_order_release);
        frame->pin_count.store(0, std::memory_order_release);
        frame->io_state = IoState::READY;
        free_list_.push_back(frame_id);
        dirty_write_backs_.fetch_add(1, std::memory_order_relaxed);
    }
    frame->condition.notify_all();
    return true;
}

void BufferPoolManager::ReleaseGuard(void* owner, Frame* frame, bool dirty,
                                     std::thread::id origin_thread) noexcept {
    static_cast<BufferPoolManager*>(owner)->ReleaseFrame(frame, dirty, origin_thread);
}

void BufferPoolManager::RegisterPinLocked(std::thread::id origin_thread) {
    ++thread_pins_[origin_thread];
}

void BufferPoolManager::ReleaseFrame(Frame* frame, bool dirty,
                                     std::thread::id origin_thread) noexcept {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    if (dirty) {
        frame->dirty.store(true, std::memory_order_release);
    }
    const int previous = frame->pin_count.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1) {
        const auto frame_id = static_cast<frame_id_t>(frame - frames_.data());
        replacer_->Unpin(frame_id);
    }
    const auto thread_pin = thread_pins_.find(origin_thread);
    if (thread_pin != thread_pins_.end()) {
        if (thread_pin->second <= 1) {
            thread_pins_.erase(thread_pin);
        } else {
            --thread_pin->second;
        }
    }
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    return page_allocator_->DeletePage(page_id);
}

void BufferPoolManager::SetWriteBackPolicy(std::shared_ptr<WriteBackPolicy> policy) {
    if (!policy) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Write-back policy cannot be null");
    }
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    write_back_policy_ =
        std::make_shared<TransactionAwareWriteBackPolicy>(*this, std::move(policy));
}

void BufferPoolManager::FlushFrame(Frame* frame) {
    std::shared_ptr<WriteBackPolicy> policy;
    WriteAheadLog* wal = nullptr;
    {
        std::lock_guard<RankedMutex> lock(pool_mutex_);
        policy = write_back_policy_;
        wal = wal_;
    }

    const page_id_t page_id = frame->page_id;
    if (!policy->CanWriteBack(page_id)) {
        return;
    }
    uint64_t lsn = 0;
    std::unique_lock<RankedSharedMutex> page_lock(frame->rwlatch, std::defer_lock);
    {
        std::lock_guard<RankedMutex> lock(pool_mutex_);
        page_lock.lock();
        if (frame->page_id != page_id || !frame->dirty.load(std::memory_order_acquire)) {
            return;
        }
        lsn = frame->lsn;
        if (active_transaction_) {
            std::lock_guard<RankedMutex> transaction_lock(active_transaction_->state_mutex_);
            if (active_transaction_->before_images_.find(page_id) !=
                active_transaction_->before_images_.end()) {
                return;
            }
        }
    }
    policy->BeforeWriteBack(page_id, lsn);
    if (wal != nullptr) {
        wal->EnsureDurable(lsn);
    }
    if (page_id == 0) {
        disk_manager_->WriteHeaderPage(frame->data.data(), frame->page_flags, frame->lsn);
    } else {
        disk_manager_->WritePage(page_id, frame->data.data(), frame->page_type, frame->page_flags,
                                 frame->lsn);
    }
    frame->dirty.store(false, std::memory_order_release);
    dirty_write_backs_.fetch_add(1, std::memory_order_relaxed);
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
    PageGuard guard;
    Frame* selected = nullptr;
    {
        std::unique_lock<RankedMutex> lock(pool_mutex_);
        for (;;) {
            const auto mapping = page_table_.find(page_id);
            if (mapping == page_table_.end()) {
                return false;
            }
            Frame& frame = frames_[static_cast<size_t>(mapping->second)];
            if (frame.io_state != IoState::READY) {
                waits_.fetch_add(1, std::memory_order_relaxed);
                frame.condition.wait(lock, [&]() {
                    const auto current = page_table_.find(page_id);
                    return current == page_table_.end() ||
                           frames_[static_cast<size_t>(current->second)].io_state == IoState::READY;
                });
                continue;
            }
            frame.pin_count.fetch_add(1, std::memory_order_relaxed);
            replacer_->Pin(mapping->second);
            RegisterPinLocked(std::this_thread::get_id());
            selected = &frame;
            guard = PageGuard(&frame, mapping->second, this, &BufferPoolManager::ReleaseGuard,
                              GuardMode::BASIC, &BufferPoolManager::RegisterMutation);
            break;
        }
    }
    FlushFrame(selected);
    guard.Drop();
    return true;
}

void BufferPoolManager::FlushAll() {
    std::vector<std::pair<Frame*, PageGuard>> pinned;
    pinned.reserve(frames_.size());
    {
        std::unique_lock<RankedMutex> lock(pool_mutex_);
        if (active_transaction_) {
            throw DbError(ErrorCode::TXN_TOO_LARGE,
                          "Cannot flush the buffer pool during an active transaction");
        }
        for (size_t index = 0; index < frames_.size(); ++index) {
            Frame& frame = frames_[index];
            while (frame.io_state != IoState::READY) {
                waits_.fetch_add(1, std::memory_order_relaxed);
                frame.condition.wait(lock);
            }
            if (frame.page_id == INVALID_PAGE_ID || frame.io_state != IoState::READY) {
                continue;
            }
            frame.pin_count.fetch_add(1, std::memory_order_relaxed);
            replacer_->Pin(static_cast<frame_id_t>(index));
            RegisterPinLocked(std::this_thread::get_id());
            pinned.emplace_back(&frame,
                                PageGuard(&frame, static_cast<frame_id_t>(index), this,
                                          &BufferPoolManager::ReleaseGuard, GuardMode::BASIC,
                                          &BufferPoolManager::RegisterMutation));
        }
    }

    for (auto& entry : pinned) {
        FlushFrame(entry.first);
        entry.second.Drop();
    }
    disk_manager_->Sync();
}

void BufferPoolManager::VerifyDiskPages() const {
    std::array<char, PAGE_DATA_SIZE> page{};
    const uint64_t page_count = disk_manager_->SizeInPages();
    for (uint64_t page_id = 0; page_id < page_count; ++page_id) {
        disk_manager_->ReadPage(static_cast<page_id_t>(page_id), page.data());
    }
}

BufferPoolStats BufferPoolManager::SnapshotStats() const {
    BufferPoolStats stats{};
    stats.hits = hits_.load(std::memory_order_relaxed);
    stats.misses = misses_.load(std::memory_order_relaxed);
    stats.evictions = evictions_.load(std::memory_order_relaxed);
    stats.dirty_write_backs = dirty_write_backs_.load(std::memory_order_relaxed);
    stats.waits = waits_.load(std::memory_order_relaxed);
    return stats;
}

std::vector<FrameInfo> BufferPoolManager::Snapshot() const {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    const size_t count = std::min(frames_.size(), size_t{4096});
    std::vector<FrameInfo> snapshot;
    snapshot.reserve(count);
    const size_t hand = replacer_->ClockHand();
    for (size_t index = 0; index < count; ++index) {
        const Frame& frame = frames_[index];
        snapshot.push_back(FrameInfo{static_cast<frame_id_t>(index), frame.page_id,
                                     frame.pin_count.load(std::memory_order_relaxed),
                                     frame.dirty.load(std::memory_order_relaxed),
                                     replacer_->ReferenceBit(static_cast<frame_id_t>(index)),
                                     hand});
    }
    return snapshot;
}

size_t BufferPoolManager::pinned_frames() const {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    return static_cast<size_t>(std::count_if(
        frames_.begin(), frames_.end(),
        [](const Frame& frame) { return frame.pin_count.load(std::memory_order_relaxed) > 0; }));
}

size_t BufferPoolManager::FreePageCount() const {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    return free_pages_.size();
}

void BufferPoolManager::AssertNoPinsHeld() const {
    std::lock_guard<RankedMutex> lock(pool_mutex_);
    const auto pins = thread_pins_.find(std::this_thread::get_id());
    if (pins != thread_pins_.end() && pins->second != 0) {
        throw DbError(
            ErrorCode::INVALID_ARGUMENT,
            "Current thread still owns " + std::to_string(pins->second) + " buffer-pool pin(s)");
    }
}

}  // namespace relational_db
