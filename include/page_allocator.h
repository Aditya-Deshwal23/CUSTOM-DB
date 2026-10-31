#ifndef PAGE_ALLOCATOR_H
#define PAGE_ALLOCATOR_H

#include <mutex>

#include "page_guard.h"

namespace relational_db {

class BufferPoolManager;

class PageAllocator {
   public:
    explicit PageAllocator(BufferPoolManager& buffer_pool) : buffer_pool_(buffer_pool) {}

    void LoadFreeList();
    WritePageGuard NewPage();
    bool DeletePage(page_id_t page_id);

   private:
    BufferPoolManager& buffer_pool_;
    std::mutex allocator_mutex_;
};

}  // namespace relational_db

#endif
