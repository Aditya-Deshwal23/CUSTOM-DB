#include "buffer_pool_manager.h"
#include <mutex>
#include <shared_mutex>

namespace relational_db {

BufferPoolManager::BufferPoolManager(size_t pool_size, std::shared_ptr<DiskManager> disk_manager)
    : pool_size_(pool_size), disk_manager_(std::move(disk_manager)) {
    pages_ = new Page[pool_size_];
    replacer_.reset(new ClockReplacer(pool_size_));

    for (size_t i = 0; i < pool_size_; ++i) {
        free_list_.push_back(static_cast<frame_id_t>(i));
    }
}

BufferPoolManager::~BufferPoolManager() {
    FlushAllPages();
    delete[] pages_;
}

Page* BufferPoolManager::FetchPage(page_id_t page_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Page* page = &pages_[frame_id];
        
        page->pin_count_++;
        replacer_->Pin(frame_id);
        hits_++;
        return page;
    }
    
    misses_++;

    frame_id_t frame_id = -1;
    if (!free_list_.empty()) {
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else if (replacer_->Victim(&frame_id)) {
        Page* victim_page = &pages_[frame_id];
        if (victim_page->IsDirty()) {
            disk_manager_->WritePage(victim_page->GetPageId(), victim_page->GetData());
        }
        page_table_.erase(victim_page->GetPageId());
    } else {
        return nullptr;
    }

    Page* page = &pages_[frame_id];
    page->page_id_ = page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = false;
    disk_manager_->ReadPage(page_id, page->GetData());
    
    page_table_[page_id] = frame_id;
    replacer_->Pin(frame_id);
    
    return page;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];
    
    if (page->pin_count_ <= 0) {
        return false;
    }

    page->pin_count_--;
    if (is_dirty) {
        page->is_dirty_ = true;
    }

    if (page->pin_count_ == 0) {
        replacer_->Unpin(frame_id);
    }

    return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];
    
    if (wal_ && page->IsDirty()) {
        LogRecord record;
        record.prev_lsn = 0;
        record.type = LogRecordType::UPDATE;
        record.page_id = page->GetPageId();
        
        const uint8_t* page_data = reinterpret_cast<const uint8_t*>(page->GetData());
        record.data.assign(page_data, page_data + PAGE_SIZE);
        
        wal_->AppendLog(record);
        wal_->Flush();
    }
    
    disk_manager_->WritePage(page->GetPageId(), page->GetData());
    page->is_dirty_ = false;
    
    return true;
}

Page* BufferPoolManager::NewPage(page_id_t* page_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    
    frame_id_t frame_id = -1;
    if (!free_list_.empty()) {
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else if (replacer_->Victim(&frame_id)) {
        Page* victim_page = &pages_[frame_id];
        if (victim_page->IsDirty()) {
            disk_manager_->WritePage(victim_page->GetPageId(), victim_page->GetData());
        }
        page_table_.erase(victim_page->GetPageId());
    } else {
        return nullptr;
    }

    *page_id = disk_manager_->AllocatePage();
    
    Page* page = &pages_[frame_id];
    page->page_id_ = *page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = false;
    page->ResetMemory();
    
    page_table_[*page_id] = frame_id;
    replacer_->Pin(frame_id);
    
    return page;
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }

    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];
    
    if (page->pin_count_ > 0) {
        return false;
    }

    page_table_.erase(page_id);
    replacer_->Pin(frame_id);
    page->page_id_ = INVALID_PAGE_ID;
    page->is_dirty_ = false;
    page->ResetMemory();
    free_list_.push_back(frame_id);
    
    return true;
}

void BufferPoolManager::FlushAllPages() {
    std::unique_lock<std::shared_mutex> lock(latch_);
    
    for (auto const& pair : page_table_) {
        frame_id_t frame_id = pair.second;
        Page* page = &pages_[frame_id];
        if (page->is_dirty_) {
            disk_manager_->WritePage(page->GetPageId(), page->GetData());
            page->is_dirty_ = false;
        }
    }
}

} // namespace relational_db
