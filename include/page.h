#ifndef PAGE_H
#define PAGE_H

#include "config.h"
#include <cstring>
#include <shared_mutex>

namespace relational_db {

/**
 * @brief Page represents a block of memory (PAGE_SIZE bytes) managed by the BufferPoolManager.
 */
class Page {
public:
    Page() { ResetMemory(); }
    ~Page() = default;

    inline char* GetData() { return data_; }
    inline page_id_t GetPageId() const { return page_id_; }
    inline int GetPinCount() const { return pin_count_; }
    inline bool IsDirty() const { return is_dirty_; }

    void RLock() { page_latch_.lock_shared(); }
    void RUnlock() { page_latch_.unlock_shared(); }
    void WLock() { page_latch_.lock(); }
    void WUnlock() { page_latch_.unlock(); }

protected:
    friend class BufferPoolManager;

    inline void ResetMemory() { std::memset(data_, 0, PAGE_SIZE); }

    char data_[PAGE_SIZE]{};
    page_id_t page_id_ = INVALID_PAGE_ID;
    int pin_count_ = 0;
    bool is_dirty_ = false;

    // Explicit standard shared_mutex
    std::shared_mutex page_latch_;
};

} // namespace relational_db

#endif // PAGE_H
