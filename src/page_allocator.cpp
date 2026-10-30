#include "page_allocator.h"

#include "buffer_pool_manager.h"
#include "db_error.h"

#include <array>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace relational_db {
namespace {

constexpr size_t kPageCountOffset = 16;
constexpr size_t kFreeListHeadOffset = 24;

uint32_t ReadU32(const char* bytes) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes);
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) |
           (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t ReadU64(const char* bytes) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes);
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(data[index]) << (index * 8U);
    }
    return value;
}

void WriteU32(char* bytes, uint32_t value) {
    auto* data = reinterpret_cast<uint8_t*>(bytes);
    for (size_t index = 0; index < 4; ++index) {
        data[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

void WriteU64(char* bytes, uint64_t value) {
    auto* data = reinterpret_cast<uint8_t*>(bytes);
    for (size_t index = 0; index < 8; ++index) {
        data[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

page_id_t DecodePageId(uint32_t value) {
    if (value == UINT32_MAX) {
        return INVALID_PAGE_ID;
    }
    if (value > static_cast<uint32_t>(std::numeric_limits<page_id_t>::max())) {
        throw DbError(ErrorCode::HEADER_CORRUPT, "Free-list page ID is out of range");
    }
    return static_cast<page_id_t>(value);
}

}  // namespace

void PageAllocator::LoadFreeList() {
    int32_t free_head;
    {
        ReadPageGuard header_guard = buffer_pool_.FetchRead(0);
        const auto* header = header_guard.As<std::array<char, PAGE_DATA_SIZE>>();
        free_head = DecodePageId(ReadU32(header->data() + kFreeListHeadOffset));
    }

    std::unordered_set<page_id_t> visited;
    while (free_head != INVALID_PAGE_ID) {
        if (free_head <= 0 ||
            static_cast<uint64_t>(free_head) >= buffer_pool_.disk_manager_->SizeInPages()) {
            throw DbError(ErrorCode::HEADER_CORRUPT, "Free-list page ID is outside the database",
                          free_head);
        }
        if (!visited.insert(free_head).second) {
            throw DbError(ErrorCode::HEADER_CORRUPT, "Cycle in page free list", free_head);
        }

        const page_id_t current_page = free_head;
        {
            ReadPageGuard free_page = buffer_pool_.FetchRead(current_page);
            const auto* body = free_page.As<std::array<char, PAGE_DATA_SIZE>>();
            free_head = DecodePageId(ReadU32(body->data()));
        }
        std::lock_guard<std::mutex> lock(buffer_pool_.pool_mutex_);
        buffer_pool_.free_pages_.insert(current_page);
    }
}

WritePageGuard PageAllocator::NewPage() {
    WritePageGuard header_guard = buffer_pool_.FetchWrite(0);
    auto* header = header_guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
    const int32_t free_head = DecodePageId(ReadU32(header->data() + kFreeListHeadOffset));

    if (free_head != INVALID_PAGE_ID) {
        WritePageGuard free_page = buffer_pool_.AllocateFreePageGuarded(free_head);
        const auto* free_body = free_page.As<std::array<char, PAGE_DATA_SIZE>>();
        const int32_t next_free = DecodePageId(ReadU32(free_body->data()));
        WriteU32(header->data() + kFreeListHeadOffset, static_cast<uint32_t>(next_free));
        auto* body = free_page.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        body->fill(0);
        free_page.SetPageType(PageType::HEAP);
        return free_page;
    }

    const uint64_t page_count = ReadU64(header->data() + kPageCountOffset);
    if (page_count > static_cast<uint64_t>(std::numeric_limits<page_id_t>::max())) {
        throw DbError(ErrorCode::IO_ERROR, "Database page IDs are exhausted");
    }
    const page_id_t page_id = static_cast<page_id_t>(page_count);
    buffer_pool_.disk_manager_->Extend(1);
    WriteU64(header->data() + kPageCountOffset, page_count + 1U);
    return buffer_pool_.AllocatePageFrame(page_id);
}

bool PageAllocator::DeletePage(page_id_t page_id) {
    if (page_id <= 0 ||
        static_cast<uint64_t>(page_id) >= buffer_pool_.disk_manager_->SizeInPages()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(buffer_pool_.pool_mutex_);
        if (buffer_pool_.free_pages_.find(page_id) != buffer_pool_.free_pages_.end()) {
            return false;
        }
    }
    WritePageGuard page_guard = buffer_pool_.FetchWrite(page_id);
    WritePageGuard header_guard = buffer_pool_.FetchWrite(0);
    auto* header = header_guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
    auto* body = page_guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
    const std::array<char, PAGE_DATA_SIZE> original_body = *page_guard.As<std::array<char, PAGE_DATA_SIZE>>();
    const PageType original_type = page_guard.GetPageType();
    const uint32_t old_head = ReadU32(header->data() + kFreeListHeadOffset);
    WriteU32(body->data(), old_head);
    page_guard.SetPageType(PageType::FREE);
    try {
        if (!buffer_pool_.RetirePage(page_guard)) {
            *body = original_body;
            page_guard.SetPageType(original_type);
            return false;
        }
    } catch (...) {
        *body = original_body;
        page_guard.SetPageType(original_type);
        throw;
    }
    WriteU32(header->data() + kFreeListHeadOffset, static_cast<uint32_t>(page_id));
    return true;
}

}  // namespace relational_db
