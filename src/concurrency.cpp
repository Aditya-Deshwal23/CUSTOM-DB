#include "concurrency.h"

#include <vector>

#ifndef NDEBUG
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#endif

#include "config.h"

namespace relational_db {
namespace {

#ifndef NDEBUG
struct HeldLock {
    LockRank rank;
    const void* identity;
};

thread_local std::vector<HeldLock> held_locks;

[[noreturn]] void AbortRankInversion(LockRank held, const void* held_lock, LockRank requested,
                                     const void* requested_lock) {
    std::fprintf(stderr, "Lock-rank inversion: held rank %u at %p, requested rank %u at %p\n",
                 static_cast<unsigned>(held), held_lock, static_cast<unsigned>(requested),
                 requested_lock);
    std::abort();
}
#endif

void NotifyWaiters(std::condition_variable& readers, std::condition_variable& writers,
                   bool writer_waiting) {
    writers.notify_one();
    if (!writer_waiting) {
        readers.notify_all();
    }
}

}  // namespace

void LockRankChecker::BeforeAcquire(LockRank rank, const void* lock) {
#ifndef NDEBUG
    for (const HeldLock& held : held_locks) {
        if (static_cast<uint8_t>(rank) < static_cast<uint8_t>(held.rank)) {
            AbortRankInversion(held.rank, held.identity, rank, lock);
        }
    }
#else
    static_cast<void>(rank);
    static_cast<void>(lock);
#endif
}

void LockRankChecker::Acquired(LockRank rank, const void* lock) {
#ifndef NDEBUG
    held_locks.push_back(HeldLock{rank, lock});
#else
    static_cast<void>(rank);
    static_cast<void>(lock);
#endif
}

void LockRankChecker::Released(const void* lock) noexcept {
#ifndef NDEBUG
    const auto found = std::find_if(held_locks.rbegin(), held_locks.rend(),
                                    [lock](const HeldLock& held) { return held.identity == lock; });
    if (found == held_locks.rend()) {
        std::fprintf(stderr, "Lock-rank checker: unlocking an untracked lock\n");
        std::abort();
    }
    held_locks.erase(std::next(found).base());
#else
    static_cast<void>(lock);
#endif
}

void LockRankChecker::AssertNotHeld(LockRank rank) {
#ifndef NDEBUG
    const auto found = std::find_if(held_locks.begin(), held_locks.end(),
                                    [rank](const HeldLock& held) { return held.rank == rank; });
    if (found != held_locks.end()) {
        std::fprintf(stderr, "Lock-rank checker: operation attempted while rank %u is held\n",
                     static_cast<unsigned>(rank));
        std::abort();
    }
#else
    static_cast<void>(rank);
#endif
}

void LockRankChecker::AssertNoLocksHeld() {
#ifndef NDEBUG
    if (!held_locks.empty()) {
        std::fprintf(stderr, "Lock-rank checker: thread exited with locks held\n");
        std::abort();
    }
#endif
}

void RankedMutex::lock() {
    LockRankChecker::BeforeAcquire(rank_, this);
    mutex_.lock();
    LockRankChecker::Acquired(rank_, this);
}

bool RankedMutex::try_lock() {
    LockRankChecker::BeforeAcquire(rank_, this);
    if (!mutex_.try_lock()) {
        return false;
    }
    LockRankChecker::Acquired(rank_, this);
    return true;
}

void RankedMutex::unlock() noexcept {
    LockRankChecker::Released(this);
    mutex_.unlock();
}

void RankedSharedMutex::lock() {
    LockRankChecker::BeforeAcquire(rank_, this);
    mutex_.lock();
    LockRankChecker::Acquired(rank_, this);
}

bool RankedSharedMutex::try_lock() {
    LockRankChecker::BeforeAcquire(rank_, this);
    if (!mutex_.try_lock()) {
        return false;
    }
    LockRankChecker::Acquired(rank_, this);
    return true;
}

void RankedSharedMutex::unlock() noexcept {
    LockRankChecker::Released(this);
    mutex_.unlock();
}

void RankedSharedMutex::lock_shared() {
    LockRankChecker::BeforeAcquire(rank_, this);
    mutex_.lock_shared();
    LockRankChecker::Acquired(rank_, this);
}

bool RankedSharedMutex::try_lock_shared() {
    LockRankChecker::BeforeAcquire(rank_, this);
    if (!mutex_.try_lock_shared()) {
        return false;
    }
    LockRankChecker::Acquired(rank_, this);
    return true;
}

void RankedSharedMutex::unlock_shared() noexcept {
    LockRankChecker::Released(this);
    mutex_.unlock_shared();
}

WriterPreferringSharedMutex::WriterPreferringSharedMutex(size_t max_concurrent, size_t max_queued,
                                                         std::chrono::milliseconds gate_timeout)
    : max_concurrent_(max_concurrent), max_queued_(max_queued), gate_timeout_(gate_timeout) {
    if (max_concurrent_ == 0 || gate_timeout_.count() <= 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Statement admission limits and timeout must be positive");
    }
}

void WriterPreferringSharedMutex::lock() {
    Lock(true, std::chrono::steady_clock::now() + gate_timeout_);
}

bool WriterPreferringSharedMutex::try_lock() {
    return TryLock(true);
}

void WriterPreferringSharedMutex::unlock() noexcept {
    Unlock(true);
}

void WriterPreferringSharedMutex::lock_shared() {
    Lock(false, std::chrono::steady_clock::now() + gate_timeout_);
}

bool WriterPreferringSharedMutex::try_lock_shared() {
    return TryLock(false);
}

void WriterPreferringSharedMutex::unlock_shared() noexcept {
    Unlock(false);
}

void WriterPreferringSharedMutex::LockUntil(bool exclusive,
                                            std::chrono::steady_clock::time_point deadline) {
    Lock(exclusive, deadline);
}

bool WriterPreferringSharedMutex::TryLock(bool exclusive) {
    LockRankChecker::BeforeAcquire(LockRank::STATEMENT_GATE, this);
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_statements_ >= max_concurrent_ || writer_active_ ||
        (exclusive && waiting_writers_ != 0) || (exclusive && active_readers_ != 0) ||
        (!exclusive && waiting_writers_ != 0)) {
        return false;
    }
    ++active_statements_;
    if (exclusive) {
        writer_active_ = true;
    } else {
        ++active_readers_;
    }
    LockRankChecker::Acquired(LockRank::STATEMENT_GATE, this);
    return true;
}

void WriterPreferringSharedMutex::Unlock(bool exclusive) noexcept {
    LockRankChecker::Released(this);
    std::lock_guard<std::mutex> lock(mutex_);
    if (exclusive) {
        writer_active_ = false;
    } else if (active_readers_ > 0) {
        --active_readers_;
    }
    if (active_statements_ > 0) {
        --active_statements_;
    }
    NotifyWaiters(readers_condition_, writers_condition_, waiting_writers_ != 0);
}

size_t WriterPreferringSharedMutex::ActiveStatements() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_statements_;
}

size_t WriterPreferringSharedMutex::QueuedStatements() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_statements_;
}

void WriterPreferringSharedMutex::Lock(bool exclusive,
                                       std::chrono::steady_clock::time_point deadline) {
    LockRankChecker::BeforeAcquire(LockRank::STATEMENT_GATE, this);
    std::unique_lock<std::mutex> lock(mutex_);
    const auto can_acquire = [this, exclusive]() {
        if (active_statements_ >= max_concurrent_ || writer_active_) {
            return false;
        }
        return exclusive ? active_readers_ == 0 : waiting_writers_ == 0;
    };
    if (!can_acquire()) {
        if (queued_statements_ >= max_queued_) {
            throw DbError(ErrorCode::BUSY, "Statement queue is full");
        }
        ++queued_statements_;
        if (exclusive) {
            ++waiting_writers_;
        }
        const bool acquired = (exclusive ? writers_condition_ : readers_condition_)
                                  .wait_until(lock, deadline, can_acquire);
        --queued_statements_;
        if (exclusive) {
            --waiting_writers_;
        }
        if (!acquired) {
            NotifyWaiters(readers_condition_, writers_condition_, waiting_writers_ != 0);
            throw DbError(ErrorCode::BUSY, "Timed out waiting for the statement gate");
        }
    }
    ++active_statements_;
    if (exclusive) {
        writer_active_ = true;
    } else {
        ++active_readers_;
    }
    lock.unlock();
    LockRankChecker::Acquired(LockRank::STATEMENT_GATE, this);
}

CancellationToken::CancellationToken(std::chrono::steady_clock::time_point deadline)
    : deadline_(deadline) {}

void CancellationToken::Cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
}

void CancellationToken::Check() const {
    if (cancelled_.load(std::memory_order_acquire) ||
        std::chrono::steady_clock::now() >= deadline_) {
        throw DbError(ErrorCode::QUERY_TIMEOUT, "Statement execution timed out");
    }
}

StatementScope::StatementScope(WriterPreferringSharedMutex& gate, bool exclusive)
    : StatementScope(
          gate, exclusive,
          std::chrono::steady_clock::now() + std::chrono::milliseconds(STATEMENT_TIMEOUT_MS)) {}

StatementScope::StatementScope(WriterPreferringSharedMutex& gate, bool exclusive,
                               std::chrono::steady_clock::time_point deadline)
    : gate_(&gate), exclusive_(exclusive), cancellation_(deadline) {
    const auto gate_deadline =
        std::min(deadline, std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(STATEMENT_GATE_TIMEOUT_MS));
    gate_->LockUntil(exclusive_, gate_deadline);
}

StatementScope::~StatementScope() {
    Unlock();
}

void StatementScope::Unlock() noexcept {
    if (gate_ != nullptr) {
        gate_->Unlock(exclusive_);
        gate_ = nullptr;
    }
}

LockRankScope::LockRankScope(LockRank rank) : identity_(this) {
    LockRankChecker::BeforeAcquire(rank, identity_);
    LockRankChecker::Acquired(rank, identity_);
}

LockRankScope::~LockRankScope() {
    LockRankChecker::Released(identity_);
}

}  // namespace relational_db
