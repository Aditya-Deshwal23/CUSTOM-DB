#ifndef RELDB_CONCURRENCY_H
#define RELDB_CONCURRENCY_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "db_error.h"

namespace relational_db {

enum class LockRank : uint8_t {
    STATEMENT_GATE = 10,
    CATALOG = 20,
    BUFFER_POOL = 30,
    FRAME_LATCH = 40,
    DISK_WAL = 50
};

class LockRankChecker {
   public:
    static void BeforeAcquire(LockRank rank, const void* lock);
    static void Acquired(LockRank rank, const void* lock);
    static void Released(const void* lock) noexcept;
    static void AssertNotHeld(LockRank rank);
    static void AssertNoLocksHeld();
};

class RankedMutex {
   public:
    explicit RankedMutex(LockRank rank) noexcept : rank_(rank) {}
    RankedMutex(const RankedMutex&) = delete;
    RankedMutex& operator=(const RankedMutex&) = delete;

    void lock();
    bool try_lock();
    void unlock() noexcept;

   private:
    std::mutex mutex_;
    LockRank rank_;
};

class RankedSharedMutex {
   public:
    explicit RankedSharedMutex(LockRank rank) noexcept : rank_(rank) {}
    RankedSharedMutex(const RankedSharedMutex&) = delete;
    RankedSharedMutex& operator=(const RankedSharedMutex&) = delete;

    void lock();
    bool try_lock();
    void unlock() noexcept;
    void lock_shared();
    bool try_lock_shared();
    void unlock_shared() noexcept;

   private:
    std::shared_mutex mutex_;
    LockRank rank_;
};

class WriterPreferringSharedMutex {
   public:
    explicit WriterPreferringSharedMutex(size_t max_concurrent = MAX_CONCURRENT_STATEMENTS,
                                         size_t max_queued = MAX_QUEUED_STATEMENTS,
                                         std::chrono::milliseconds gate_timeout =
                                             std::chrono::milliseconds(STATEMENT_GATE_TIMEOUT_MS));

    WriterPreferringSharedMutex(const WriterPreferringSharedMutex&) = delete;
    WriterPreferringSharedMutex& operator=(const WriterPreferringSharedMutex&) = delete;

    void lock();
    bool try_lock();
    void unlock() noexcept;
    void lock_shared();
    bool try_lock_shared();
    void unlock_shared() noexcept;
    void LockUntil(bool exclusive, std::chrono::steady_clock::time_point deadline);
    bool TryLock(bool exclusive);
    void Unlock(bool exclusive) noexcept;

    size_t ActiveStatements() const;
    size_t QueuedStatements() const;

   private:
    void Lock(bool exclusive, std::chrono::steady_clock::time_point deadline);
    mutable std::mutex mutex_;
    std::condition_variable readers_condition_;
    std::condition_variable writers_condition_;
    size_t max_concurrent_;
    size_t max_queued_;
    std::chrono::milliseconds gate_timeout_;
    size_t active_readers_{0};
    size_t active_statements_{0};
    size_t queued_statements_{0};
    size_t waiting_writers_{0};
    bool writer_active_{false};
};

class CancellationToken {
   public:
    explicit CancellationToken(std::chrono::steady_clock::time_point deadline);
    void Cancel() noexcept;
    void Check() const;

   private:
    std::atomic<bool> cancelled_{false};
    std::chrono::steady_clock::time_point deadline_;
};

class StatementScope {
   public:
    StatementScope(WriterPreferringSharedMutex& gate, bool exclusive);
    StatementScope(WriterPreferringSharedMutex& gate, bool exclusive,
                   std::chrono::steady_clock::time_point deadline);
    ~StatementScope();
    StatementScope(const StatementScope&) = delete;
    StatementScope& operator=(const StatementScope&) = delete;

    CancellationToken& Cancellation() noexcept {
        return cancellation_;
    }

    void Unlock() noexcept;
    bool Exclusive() const noexcept {
        return exclusive_;
    }

   private:
    WriterPreferringSharedMutex* gate_;
    bool exclusive_;
    CancellationToken cancellation_;
};

class LockRankScope {
   public:
    explicit LockRankScope(LockRank rank);
    ~LockRankScope();
    LockRankScope(const LockRankScope&) = delete;
    LockRankScope& operator=(const LockRankScope&) = delete;

   private:
    const void* identity_;
};

}  // namespace relational_db

#endif
