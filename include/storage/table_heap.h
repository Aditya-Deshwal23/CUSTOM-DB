#ifndef RELDB_STORAGE_TABLE_HEAP_H
#define RELDB_STORAGE_TABLE_HEAP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>
#include <utility>

#include "buffer_pool_manager.h"
#include "storage/tuple.h"

namespace relational_db {

class SlottedPage;

struct RID {
    page_id_t page_id{INVALID_PAGE_ID};
    uint16_t slot{0};

    uint64_t Pack() const;
    static RID Unpack(uint64_t packed);

    bool operator==(const RID& other) const noexcept {
        return page_id == other.page_id && slot == other.slot;
    }
    bool operator!=(const RID& other) const noexcept {
        return !(*this == other);
    }
};

class TableIterator {
   public:
    TableIterator() = default;
    TableIterator(BufferPoolManager& buffer_pool, page_id_t first_page_id);
    TableIterator(TableIterator&&) noexcept = default;
    TableIterator& operator=(TableIterator&&) noexcept = default;
    TableIterator(const TableIterator&) = delete;
    TableIterator& operator=(const TableIterator&) = delete;

    bool Next(RID& rid, Tuple& tuple);

   private:
    BufferPoolManager* buffer_pool_{nullptr};
    page_id_t next_page_id_{INVALID_PAGE_ID};
    page_id_t current_page_id_{INVALID_PAGE_ID};
    uint16_t next_slot_{0};
    ReadPageGuard page_guard_;
};

class TableHeap {
   public:
    static TableHeap Create(BufferPoolManager& buffer_pool);

    explicit TableHeap(BufferPoolManager& buffer_pool, page_id_t header_page_id);

    page_id_t HeaderPageId() const noexcept {
        return header_page_id_;
    }
    RID Insert(const Tuple& tuple);
    std::optional<Tuple> Get(const RID& rid) const;
    bool Delete(const RID& rid);
    std::optional<RID> Update(const RID& rid, const Tuple& tuple);
    TableIterator Begin() const;
    void Drop();

   private:
    static constexpr size_t MAX_RECLAIMED_PAGES = 32;

    BufferPoolManager& buffer_pool_;
    page_id_t header_page_id_;
    mutable std::unordered_set<page_id_t> reclaimed_pages_;
    mutable bool reclaimed_pages_built_{false};

    std::pair<page_id_t, page_id_t> ReadHeader() const;
    void WriteHeader(page_id_t first_page_id, page_id_t last_page_id);
    void RebuildReclaimedPages() const;
    void RememberReclaimedPage(page_id_t page_id);
    void RefreshReclaimedPage(page_id_t page_id, const SlottedPage& page);
    RID InsertIntoNewPage(const Tuple& tuple, page_id_t first_page_id, page_id_t last_page_id);
};

}  // namespace relational_db

namespace std {
template <>
struct hash<relational_db::RID> {
    size_t operator()(const relational_db::RID& rid) const noexcept {
        return hash<uint64_t>{}((static_cast<uint64_t>(static_cast<uint32_t>(rid.page_id)) << 16U) |
                                rid.slot);
    }
};
}  // namespace std

#endif
