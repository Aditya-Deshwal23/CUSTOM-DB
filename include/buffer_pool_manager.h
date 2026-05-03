#ifndef BUFFER_POOL_MANAGER_H
#define BUFFER_POOL_MANAGER_H

#include "config.h"
#include "disk_manager.h"
#include "clock_replacer.h"
#include "page.h"
#include "wal.h"
#include <list>
#include <unordered_map>
#include <memory>
#include <shared_mutex>

namespace relational_db {

class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, std::shared_ptr<DiskManager> disk_manager);
    ~BufferPoolManager();

    Page* FetchPage(page_id_t page_id);
    bool UnpinPage(page_id_t page_id, bool is_dirty);
    bool FlushPage(page_id_t page_id);
    Page* NewPage(page_id_t* page_id);
    bool DeletePage(page_id_t page_id);
    void FlushAllPages();
    void SetWAL(std::shared_ptr<WriteAheadLog> wal) { wal_ = std::move(wal); }

    int GetHits() const { return hits_; }
    int GetMisses() const { return misses_; }

private:
    std::atomic<int> hits_{0};
    std::atomic<int> misses_{0};
    
    size_t pool_size_;
    std::shared_ptr<DiskManager> disk_manager_;
    std::unique_ptr<ClockReplacer> replacer_;
    Page* pages_;

    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::list<frame_id_t> free_list_;

    // Explicit standard shared_mutex
    mutable std::shared_mutex latch_;

    std::shared_ptr<WriteAheadLog> wal_;
};

} // namespace relational_db

#endif // BUFFER_POOL_MANAGER_H
