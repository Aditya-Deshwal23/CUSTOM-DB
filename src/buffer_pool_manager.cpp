#include "buffer_pool_manager.h"

#include "db_error.h"
#include "page_allocator.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace relational_db {
namespace {

size_t ValidatePoolSize(size_t pool_size) {
    if (pool_size == 0 ||
        pool_size > static_cast<size_t>(std::numeric_limits<frame_id_t>::max())) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Buffer-pool frame count is out of range");
    }
    return pool_size;
}

}  // namespace

size_t BufferPoolManager::ConfiguredPoolSize() {
    constexpr size_t kDefaultFrames = 4096;
    const char* configured = std::getenv("BUFFER_POOL_FRAMES");
    if (configured == nullptr || *configured == '\0') {
        return kDefaultFrames;
    }

    size_t frames = 0;
    const char* end = configured;
    while (*end != '\0') {
        ++end;
    }
    const auto parsed = std::from_chars(configured, end, frames);
    if (parsed.ec != std::errc{} || parsed.ptr != end || frames < 16 ||
        frames > static_cast<size_t>(std::numeric_limits<frame_id_t>::max())) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "BUFFER_POOL_FRAMES must be an integer between 16 and " +
                          std::to_string(std::numeric_limits<frame_id_t>::max()));
    }
    return frames;
}

BufferPoolManager::BufferPoolManager(std::shared_ptr<IDiskManager> disk_manager)
    : BufferPoolManager(ConfiguredPoolSize(), std::move(disk_manager)) {}

BufferPoolManager::BufferPoolManager(size_t pool_size, std::shared_ptr<IDiskManager> disk_manager)
    : frames_(ValidatePoolSize(pool_size)),
      disk_manager_(std::move(disk_manager)),
      write_back_policy_(std::make_shared<AllowAllWriteBackPolicy>()) {
    if (!disk_manager_) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Buffer pool requires a disk manager");
    }

    const char* policy = std::getenv("BUFFER_POOL_POLICY");
    const std::string policy_name = policy == nullptr ? "lruk" : policy;
    if (policy_name == "clock") {
        replacer_ = std::make_unique<ClockReplacer>(pool_size);
    } else if (policy_name == "lruk") {
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
        std::cerr << "BufferPoolManager shutdown flush failed: " << error.what() << '\n';
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

PageGuard BufferPoolManager::Fetch(page_id_t page_id, GuardMode mode, bool allow_free_page) {
    if (page_id < 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Page ID cannot be negative", page_id);
    }

    bool recorded_miss = false;
    for (;;) {
        Frame* frame = nullptr;
        frame_id_t frame_id = -1;
        page_id_t evicted_page_id = INVALID_PAGE_ID;
        bool evicted_dirty = false;
        uint64_t evicted_lsn = 0;
        std::array<char, PAGE_DATA_SIZE> evicted_data{};
        bool claimed_free_page = false;
        std::shared_ptr<WriteBackPolicy> policy;
        {
            std::unique_lock<std::mutex> lock(pool_mutex_);
            const auto write_back = in_flight_write_backs_.find(page_id);
            if (write_back != in_flight_write_backs_.end()) {
                Frame& pending = frames_[static_cast<size_t>(write_back->second)];
                waits_.fetch_add(1, std::memory_order_relaxed);
                pending.condition.wait(lock, [&]() {
                    return in_flight_write_backs_.find(page_id) ==
                           in_flight_write_backs_.end();
                });
                continue;
            }
            const auto free_page = free_pages_.find(page_id);
            if (free_page != free_pages_.end()) {
                if (!allow_free_page) {
                    throw DbError(ErrorCode::PAGE_OUT_OF_RANGE,
                                  "Page ID has been deleted and is on the free list", page_id);
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
                        return mapping == page_table_.end() ||
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
                }
                return PageGuard(frame, frame_id, this, &BufferPoolManager::ReleaseGuard, mode);
            }

            if (!recorded_miss) {
                misses_.fetch_add(1, std::memory_order_relaxed);
                recorded_miss = true;
            }
            if (!free_list_.empty()) {
                frame_id = free_list_.back();
                free_list_.pop_back();
            } else if (!replacer_->Victim(&frame_id)) {
                if (claimed_free_page) {
                    free_pages_.insert(page_id);
                }
                throw DbError(ErrorCode::BUFFER_POOL_EXHAUSTED,
                              "All buffer-pool frames are pinned or unavailable", page_id);
            }
            frame = &frames_[static_cast<size_t>(frame_id)];
            evicted_page_id = frame->page_id;
            evicted_dirty = evicted_page_id != INVALID_PAGE_ID &&
                            frame->dirty.load(std::memory_order_acquire);
            evicted_lsn = frame->lsn;
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
            if (!evicted_dirty) {
                frame->dirty.store(false, std::memory_order_release);
            }
            page_table_[page_id] = frame_id;
            policy = write_back_policy_;
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
                if (evicted_page_id == 0) {
                    disk_manager_->WriteHeaderPage(frame->data.data());
                } else {
                    disk_manager_->WritePage(evicted_page_id, frame->data.data());
                }
                write_back_completed = true;
                dirty_write_backs_.fetch_add(1, std::memory_order_relaxed);
                frame->dirty.store(false, std::memory_order_release);
                std::lock_guard<std::mutex> lock(pool_mutex_);
                frame->io_state = IoState::LOADING;
            }
            disk_manager_->ReadPage(page_id, frame->data.data());
            frame->lsn = 0;
            if (mode == GuardMode::READ) {
                frame->rwlatch.lock_shared();
                latch_acquired = true;
            } else if (mode == GuardMode::WRITE) {
                frame->rwlatch.lock();
                latch_acquired = true;
            }
            {
                std::lock_guard<std::mutex> lock(pool_mutex_);
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
            return PageGuard(frame, frame_id, this, &BufferPoolManager::ReleaseGuard, mode);
        } catch (...) {
            if (latch_acquired && mode == GuardMode::READ) {
                frame->rwlatch.unlock_shared();
            } else if (latch_acquired && mode == GuardMode::WRITE) {
                frame->rwlatch.unlock();
            }
            std::lock_guard<std::mutex> lock(pool_mutex_);
            const auto mapping = page_table_.find(page_id);
            if (mapping != page_table_.end() && mapping->second == frame_id) {
                page_table_.erase(mapping);
            }
            if (evicted_dirty) {
                in_flight_write_backs_.erase(evicted_page_id);
            }
            frame->pin_count.store(0, std::memory_order_release);
            frame->page_id = evicted_page_id;
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

bool BufferPoolManager::RetirePage(WritePageGuard& guard) {
    Frame* frame = guard.FramePointer();
    const page_id_t page_id = guard.GetPageId();
    const frame_id_t frame_id = guard.GetFrameId();
    std::shared_ptr<WriteBackPolicy> policy;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        const auto mapping = page_table_.find(page_id);
        if (mapping == page_table_.end() || mapping->second != frame_id ||
            frame->pin_count.load(std::memory_order_acquire) != 1 ||
            frame->io_state != IoState::READY) {
            return false;
        }
        free_pages_.insert(page_id);
        frame->io_state = IoState::WRITING_BACK;
        policy = write_back_policy_;
    }

    try {
        if (!policy->CanWriteBack(page_id)) {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            free_pages_.erase(page_id);
            frame->io_state = IoState::READY;
            frame->condition.notify_all();
            return false;
        }
        policy->BeforeWriteBack(page_id, frame->lsn);
        disk_manager_->WritePage(page_id, frame->data.data());
    } catch (...) {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        free_pages_.erase(page_id);
        frame->io_state = IoState::READY;
        frame->condition.notify_all();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        page_table_.erase(page_id);
        frame->page_id = INVALID_PAGE_ID;
        frame->dirty.store(false, std::memory_order_release);
        frame->pin_count.store(0, std::memory_order_release);
        frame->io_state = IoState::READY;
        free_list_.push_back(frame_id);
        replacer_->Pin(frame_id);
        const auto thread_pin = thread_pins_.find(guard.origin_thread_);
        if (thread_pin != thread_pins_.end()) {
            if (thread_pin->second <= 1) {
                thread_pins_.erase(thread_pin);
            } else {
                --thread_pin->second;
            }
        }
        dirty_write_backs_.fetch_add(1, std::memory_order_relaxed);
    }
    guard.Disarm();
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
    std::lock_guard<std::mutex> lock(pool_mutex_);
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
    std::lock_guard<std::mutex> lock(pool_mutex_);
    write_back_policy_ = std::move(policy);
}

void BufferPoolManager::FlushFrame(Frame* frame) {
    std::unique_lock<std::shared_mutex> page_lock(frame->rwlatch);
    if (!frame->dirty.load(std::memory_order_acquire)) {
        return;
    }

    page_id_t page_id;
    uint64_t lsn;
    std::shared_ptr<WriteBackPolicy> policy;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        page_id = frame->page_id;
        lsn = frame->lsn;
        policy = write_back_policy_;
    }
    if (!policy->CanWriteBack(page_id)) {
        return;
    }
    policy->BeforeWriteBack(page_id, lsn);
    if (page_id == 0) {
        disk_manager_->WriteHeaderPage(frame->data.data());
    } else {
        disk_manager_->WritePage(page_id, frame->data.data());
    }
    frame->dirty.store(false, std::memory_order_release);
    dirty_write_backs_.fetch_add(1, std::memory_order_relaxed);
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
    PageGuard guard;
    Frame* selected = nullptr;
    {
        std::unique_lock<std::mutex> lock(pool_mutex_);
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
                              GuardMode::BASIC);
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
        std::unique_lock<std::mutex> lock(pool_mutex_);
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
            pinned.emplace_back(
                &frame, PageGuard(&frame, static_cast<frame_id_t>(index), this,
                                  &BufferPoolManager::ReleaseGuard, GuardMode::BASIC));
        }
    }

    for (auto& entry : pinned) {
        FlushFrame(entry.first);
        entry.second.Drop();
    }
    disk_manager_->Sync();
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
    std::lock_guard<std::mutex> lock(pool_mutex_);
    const size_t count = std::min(frames_.size(), size_t{4096});
    std::vector<FrameInfo> snapshot;
    snapshot.reserve(count);
    const size_t hand = replacer_->ClockHand();
    for (size_t index = 0; index < count; ++index) {
        const Frame& frame = frames_[index];
        snapshot.push_back(FrameInfo{static_cast<frame_id_t>(index), frame.page_id,
                                     frame.pin_count.load(std::memory_order_relaxed),
                                     frame.dirty.load(std::memory_order_relaxed),
                                     replacer_->ReferenceBit(static_cast<frame_id_t>(index)), hand});
    }
    return snapshot;
}

size_t BufferPoolManager::pinned_frames() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return static_cast<size_t>(std::count_if(frames_.begin(), frames_.end(), [](const Frame& frame) {
        return frame.pin_count.load(std::memory_order_relaxed) > 0;
    }));
}

void BufferPoolManager::AssertNoPinsHeld() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    const auto pins = thread_pins_.find(std::this_thread::get_id());
    if (pins != thread_pins_.end() && pins->second != 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Current thread still owns " + std::to_string(pins->second) +
                          " buffer-pool pin(s)");
    }
}

}  // namespace relational_db
