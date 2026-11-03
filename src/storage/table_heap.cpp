#include "storage/table_heap.h"

#include <array>
#include <limits>
#include <unordered_set>
#include <vector>

#include "db_error.h"
#include "slotted_page.h"

namespace relational_db {
namespace {

uint32_t ReadU32(const char* bytes, size_t offset) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes + offset);
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

void WriteU32(char* bytes, size_t offset, uint32_t value) {
    auto* data = reinterpret_cast<uint8_t*>(bytes + offset);
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
        data[byte] = static_cast<uint8_t>(value >> (byte * 8U));
    }
}

page_id_t DecodePageId(uint32_t encoded) {
    if (encoded == UINT32_MAX) {
        return INVALID_PAGE_ID;
    }
    if (encoded == 0 || encoded > static_cast<uint32_t>(std::numeric_limits<page_id_t>::max())) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap page ID is out of range");
    }
    return static_cast<page_id_t>(encoded);
}

void ValidatePageLink(page_id_t page_id) {
    if (page_id < INVALID_PAGE_ID || page_id == 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Invalid table-heap page ID", page_id);
    }
}

}  // namespace

uint64_t RID::Pack() const {
    if (page_id <= 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Cannot pack an invalid RID", page_id);
    }
    return (static_cast<uint64_t>(static_cast<uint32_t>(page_id)) << 16U) |
           static_cast<uint64_t>(slot);
}

RID RID::Unpack(uint64_t packed) {
    if ((packed >> 48U) != 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Packed RID has nonzero reserved bits");
    }
    const uint32_t encoded_page = static_cast<uint32_t>(packed >> 16U);
    if (encoded_page == 0 ||
        encoded_page > static_cast<uint32_t>(std::numeric_limits<page_id_t>::max())) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Packed RID contains an invalid page ID");
    }
    return RID{static_cast<page_id_t>(encoded_page), static_cast<uint16_t>(packed)};
}

TableIterator::TableIterator(BufferPoolManager& buffer_pool, page_id_t first_page_id)
    : buffer_pool_(&buffer_pool), next_page_id_(first_page_id) {}

bool TableIterator::Next(RID& rid, Tuple& tuple) {
    if (buffer_pool_ == nullptr) {
        return false;
    }
    for (;;) {
        if (current_page_id_ == INVALID_PAGE_ID) {
            if (next_page_id_ == INVALID_PAGE_ID) {
                return false;
            }
            if (next_page_id_ <= 0) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid page in table-heap scan",
                              next_page_id_);
            }
            current_page_id_ = next_page_id_;
            next_slot_ = 0;
            page_loaded_ = false;
        }
        if (current_page_id_ <= 0) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid page in table-heap scan",
                          current_page_id_);
        }
        if (!page_loaded_) {
            ReadPageGuard page_guard = buffer_pool_->FetchRead(current_page_id_);
            page_data_ = *page_guard.As<std::array<char, PAGE_DATA_SIZE>>();
            page_loaded_ = true;
        }

        const SlottedPage page(page_data_.data());
        const uint16_t slot_count = page.GetNumSlots();
        while (next_slot_ < slot_count) {
            const uint16_t slot_id = next_slot_++;
            if (page.ReadTuple(slot_id, tuple)) {
                rid = RID{current_page_id_, slot_id};
                return true;
            }
        }
        next_page_id_ = page.GetNextPageId();
        current_page_id_ = INVALID_PAGE_ID;
        page_loaded_ = false;
        if (next_page_id_ != INVALID_PAGE_ID && next_page_id_ <= 0) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid next page in table-heap scan",
                          next_page_id_);
        }
    }
}

TableHeap TableHeap::Create(BufferPoolManager& buffer_pool) {
    page_id_t header_page_id = INVALID_PAGE_ID;
    {
        WritePageGuard guard = buffer_pool.NewPageGuarded();
        header_page_id = guard.GetPageId();
        auto* body = guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        body->fill(0);
        WriteU32(body->data(), 0, UINT32_MAX);
        WriteU32(body->data(), sizeof(uint32_t), UINT32_MAX);
    }
    return TableHeap(buffer_pool, header_page_id);
}

TableHeap::TableHeap(BufferPoolManager& buffer_pool, page_id_t header_page_id)
    : buffer_pool_(buffer_pool), header_page_id_(header_page_id) {
    if (header_page_id_ <= 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Table heap requires a valid header page",
                      header_page_id_);
    }
    static_cast<void>(ReadHeader());
}

std::pair<page_id_t, page_id_t> TableHeap::ReadHeader() const {
    ReadPageGuard guard = buffer_pool_.FetchRead(header_page_id_);
    const auto* body = guard.As<std::array<char, PAGE_DATA_SIZE>>();
    const page_id_t first = DecodePageId(ReadU32(body->data(), 0));
    const page_id_t last = DecodePageId(ReadU32(body->data(), sizeof(uint32_t)));
    if ((first == INVALID_PAGE_ID) != (last == INVALID_PAGE_ID)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap header has incomplete page links",
                      header_page_id_);
    }
    if (first != INVALID_PAGE_ID && (first <= 0 || last <= 0)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap header contains invalid page links",
                      header_page_id_);
    }
    return {first, last};
}

void TableHeap::WriteHeader(page_id_t first_page_id, page_id_t last_page_id) {
    ValidatePageLink(first_page_id);
    ValidatePageLink(last_page_id);
    if ((first_page_id == INVALID_PAGE_ID) != (last_page_id == INVALID_PAGE_ID)) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Table heap links must both be set or empty");
    }
    WritePageGuard guard = buffer_pool_.FetchWrite(header_page_id_);
    auto* body = guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
    WriteU32(body->data(), 0, static_cast<uint32_t>(first_page_id));
    WriteU32(body->data(), sizeof(uint32_t), static_cast<uint32_t>(last_page_id));
}

void TableHeap::RememberReclaimedPage(page_id_t page_id) {
    if (reclaimed_pages_.size() < MAX_RECLAIMED_PAGES ||
        reclaimed_pages_.find(page_id) != reclaimed_pages_.end()) {
        reclaimed_pages_.insert(page_id);
    }
}

void TableHeap::RefreshReclaimedPage(page_id_t page_id, const SlottedPage& page) {
    if (page.HasTombstones() || page.GetFreeSpaceRemaining() > 0) {
        RememberReclaimedPage(page_id);
    } else {
        reclaimed_pages_.erase(page_id);
    }
}

void TableHeap::RebuildReclaimedPages() const {
    if (reclaimed_pages_built_) {
        return;
    }
    reclaimed_pages_.clear();
    const auto links = ReadHeader();
    page_id_t page_id = links.first;
    std::unordered_set<page_id_t> visited;
    while (page_id != INVALID_PAGE_ID) {
        if (page_id <= 0 || !visited.insert(page_id).second) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid or cyclic table-heap page list",
                          page_id);
        }
        page_id_t next_page_id = INVALID_PAGE_ID;
        bool reclaimed = false;
        {
            ReadPageGuard guard = buffer_pool_.FetchRead(page_id);
            const auto* body = guard.As<std::array<char, PAGE_DATA_SIZE>>();
            const SlottedPage page(body->data());
            next_page_id = page.GetNextPageId();
            reclaimed = page.HasTombstones() || page.GetFreeSpaceRemaining() > 0;
        }
        if (reclaimed && page_id != links.second && reclaimed_pages_.size() < MAX_RECLAIMED_PAGES) {
            reclaimed_pages_.insert(page_id);
        }
        page_id = next_page_id;
    }
    reclaimed_pages_built_ = true;
}

RID TableHeap::InsertIntoNewPage(const Tuple& tuple, page_id_t first_page_id,
                                 page_id_t last_page_id) {
    page_id_t new_page_id = INVALID_PAGE_ID;
    uint16_t slot_id = SlottedPage::INVALID_SLOT_ID;
    {
        WritePageGuard guard = buffer_pool_.NewPageGuarded();
        new_page_id = guard.GetPageId();
        auto* body = guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        SlottedPage page(body->data());
        page.Init();
        slot_id = page.InsertTuple(tuple.data(), tuple.size());
        if (slot_id == SlottedPage::INVALID_SLOT_ID) {
            throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Tuple cannot fit in an empty heap page",
                          new_page_id);
        }
    }

    if (last_page_id != INVALID_PAGE_ID) {
        WritePageGuard previous_guard = buffer_pool_.FetchWrite(last_page_id);
        auto* previous_body = previous_guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        SlottedPage previous_page(previous_body->data());
        if (previous_page.GetNextPageId() != INVALID_PAGE_ID) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Heap tail already has a next page",
                          last_page_id);
        }
        previous_page.SetNextPageId(new_page_id);
    } else {
        first_page_id = new_page_id;
    }
    WriteHeader(first_page_id, new_page_id);
    reclaimed_pages_built_ = true;
    return RID{new_page_id, slot_id};
}

RID TableHeap::Insert(const Tuple& tuple) {
    if (tuple.size() > MAX_TUPLE_SIZE) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Tuple exceeds MAX_TUPLE_SIZE");
    }
    const auto links = ReadHeader();
    if (links.second != INVALID_PAGE_ID) {
        WritePageGuard guard = buffer_pool_.FetchWrite(links.second);
        auto* body = guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        SlottedPage page(body->data());
        const uint16_t slot_id = page.InsertTuple(tuple.data(), tuple.size());
        if (slot_id != SlottedPage::INVALID_SLOT_ID) {
            RefreshReclaimedPage(links.second, page);
            return RID{links.second, slot_id};
        }
        RefreshReclaimedPage(links.second, page);
    }

    RebuildReclaimedPages();
    std::vector<page_id_t> candidates(reclaimed_pages_.begin(), reclaimed_pages_.end());
    for (page_id_t page_id : candidates) {
        WritePageGuard guard = buffer_pool_.FetchWrite(page_id);
        auto* body = guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        SlottedPage page(body->data());
        const uint16_t slot_id = page.InsertTuple(tuple.data(), tuple.size());
        if (slot_id != SlottedPage::INVALID_SLOT_ID) {
            RefreshReclaimedPage(page_id, page);
            return RID{page_id, slot_id};
        }
        RefreshReclaimedPage(page_id, page);
    }
    return InsertIntoNewPage(tuple, links.first, links.second);
}

std::optional<Tuple> TableHeap::Get(const RID& rid) const {
    if (rid.page_id <= 0) {
        return std::nullopt;
    }
    ReadPageGuard guard = buffer_pool_.FetchRead(rid.page_id);
    const auto* body = guard.As<std::array<char, PAGE_DATA_SIZE>>();
    const SlottedPage page(body->data());
    Tuple tuple;
    if (!page.ReadTuple(rid.slot, tuple)) {
        return std::nullopt;
    }
    return tuple;
}

bool TableHeap::Delete(const RID& rid) {
    if (rid.page_id <= 0) {
        return false;
    }
    bool deleted = false;
    {
        WritePageGuard guard = buffer_pool_.FetchWrite(rid.page_id);
        auto* body = guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        SlottedPage page(body->data());
        deleted = page.DeleteTuple(rid.slot);
        if (deleted) {
            RememberReclaimedPage(rid.page_id);
        }
    }
    return deleted;
}

std::optional<RID> TableHeap::Update(const RID& rid, const Tuple& tuple) {
    if (tuple.size() > MAX_TUPLE_SIZE) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Tuple exceeds MAX_TUPLE_SIZE");
    }
    if (rid.page_id <= 0) {
        return std::nullopt;
    }
    SlottedPage::UpdateResult result = SlottedPage::UpdateResult::NOT_FOUND;
    {
        WritePageGuard guard = buffer_pool_.FetchWrite(rid.page_id);
        auto* body = guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        SlottedPage page(body->data());
        result = page.UpdateTuple(rid.slot, tuple.data(), tuple.size());
        if (result == SlottedPage::UpdateResult::UPDATED) {
            RefreshReclaimedPage(rid.page_id, page);
            return rid;
        }
    }
    if (result == SlottedPage::UpdateResult::NOT_FOUND) {
        return std::nullopt;
    }
    const RID replacement = Insert(tuple);
    if (!Delete(rid)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Tuple moved but its old RID could not be deleted",
                      rid.page_id);
    }
    return replacement;
}

TableIterator TableHeap::Begin() const {
    return TableIterator(buffer_pool_, ReadHeader().first);
}

TableHeapInspection TableHeap::Inspect(uint64_t max_pages) const {
    if (max_pages == 0U) {
        throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED,
                      "Table heap inspection exceeds INSPECT_MAX_PAGES", header_page_id_);
    }
    std::array<char, PAGE_DATA_SIZE> body{};
    PageType type = PageType::HEAP;
    uint16_t flags = 0;
    uint64_t lsn = 0;
    buffer_pool_.ReadPageWithoutReplacement(header_page_id_, body.data(), type, flags, lsn);
    if (type != PageType::HEAP) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap header has an invalid page type",
                      header_page_id_);
    }

    TableHeapInspection summary;
    summary.pages = 1;
    summary.first_page = DecodePageId(ReadU32(body.data(), 0));
    page_id_t page_id = summary.first_page;
    std::unordered_set<page_id_t> visited;
    while (page_id != INVALID_PAGE_ID) {
        if (summary.pages >= max_pages || !visited.insert(page_id).second) {
            throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED,
                          "Table heap inspection exceeds INSPECT_MAX_PAGES", page_id);
        }
        buffer_pool_.ReadPageWithoutReplacement(page_id, body.data(), type, flags, lsn);
        if (type != PageType::HEAP) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap contains a non-heap page", page_id);
        }
        const SlottedPage page(body.data());
        const uint16_t slots = page.GetNumSlots();
        for (uint16_t slot_id = 0; slot_id < slots; ++slot_id) {
            std::vector<std::byte> tuple;
            if (page.ReadTuple(slot_id, tuple)) {
                ++summary.tuples;
            }
        }
        const uint64_t free_bytes = page.GetFreeSpaceRemaining();
        summary.bytes_free += free_bytes;
        summary.bytes_used += PAGE_DATA_SIZE - free_bytes;
        page_id = page.GetNextPageId();
        ++summary.pages;
    }
    return summary;
}

TableHeapPageInspection TableHeap::InspectPage(page_id_t target_page, uint64_t max_pages) const {
    if (target_page < 0 || max_pages == 0U) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "Heap page inspection requires a valid page and positive limit", target_page);
    }
    TableHeapPageInspection result;
    std::array<char, PAGE_DATA_SIZE> body{};
    PageType type = PageType::HEAP;
    uint16_t flags = 0;
    uint64_t lsn = 0;
    buffer_pool_.ReadPageWithoutReplacement(header_page_id_, body.data(), type, flags, lsn);
    ++result.pages_read;
    if (type != PageType::HEAP) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap header has an invalid page type",
                      header_page_id_);
    }
    if (target_page == header_page_id_) {
        result.found = true;
        result.header_page = true;
        return result;
    }
    page_id_t page_id = DecodePageId(ReadU32(body.data(), 0));
    std::unordered_set<page_id_t> visited;
    while (page_id != INVALID_PAGE_ID) {
        if (result.pages_read >= max_pages) {
            throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED,
                          "Heap page lookup exceeds INSPECT_MAX_PAGES", target_page);
        }
        if (page_id <= 0 || !visited.insert(page_id).second) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid or cyclic table heap page chain",
                          page_id);
        }
        buffer_pool_.ReadPageWithoutReplacement(page_id, body.data(), type, flags, lsn);
        ++result.pages_read;
        if (type != PageType::HEAP) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap contains a non-heap page", page_id);
        }
        if (page_id == target_page) {
            const SlottedPage page(body.data());
            const uint16_t slots = page.GetNumSlots();
            result.tuples.reserve(slots);
            for (uint16_t slot = 0; slot < slots; ++slot) {
                Tuple tuple;
                if (page.ReadTuple(slot, tuple)) {
                    result.tuples.emplace_back(slot, std::move(tuple));
                }
            }
            result.found = true;
            return result;
        }
        page_id = SlottedPage(body.data()).GetNextPageId();
    }
    return result;
}

uint64_t TableHeap::ScanWithoutReplacement(
    uint64_t max_pages, uint64_t max_rows,
    const std::function<void(const RID&, const Tuple&)>& visitor) const {
    if (max_pages == 0U || max_rows == 0U) {
        throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED,
                      "Table row inspection requires positive page and row limits",
                      header_page_id_);
    }
    std::array<char, PAGE_DATA_SIZE> body{};
    PageType type = PageType::HEAP;
    uint16_t flags = 0;
    uint64_t lsn = 0;
    buffer_pool_.ReadPageWithoutReplacement(header_page_id_, body.data(), type, flags, lsn);
    if (type != PageType::HEAP) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap header has an invalid page type",
                      header_page_id_);
    }

    page_id_t page_id = DecodePageId(ReadU32(body.data(), 0));
    std::unordered_set<page_id_t> visited;
    uint64_t pages = 1;
    uint64_t rows = 0;
    while (page_id != INVALID_PAGE_ID) {
        if (pages >= max_pages || page_id <= 0 || !visited.insert(page_id).second) {
            throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED,
                          "Table row inspection exceeds configured page limits", page_id);
        }
        buffer_pool_.ReadPageWithoutReplacement(page_id, body.data(), type, flags, lsn);
        if (type != PageType::HEAP) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Table heap contains a non-heap page", page_id);
        }
        const SlottedPage page(body.data());
        const uint16_t slot_count = page.GetNumSlots();
        for (uint16_t slot = 0; slot < slot_count; ++slot) {
            Tuple tuple;
            if (!page.ReadTuple(slot, tuple)) {
                continue;
            }
            if (rows == max_rows) {
                throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED,
                              "Table row inspection exceeds configured row limits", page_id);
            }
            visitor(RID{page_id, slot}, tuple);
            ++rows;
        }
        page_id = page.GetNextPageId();
        ++pages;
    }
    return rows;
}

void TableHeap::Drop() {
    const auto links = ReadHeader();
    std::vector<page_id_t> pages;
    page_id_t page_id = links.first;
    std::unordered_set<page_id_t> visited;
    while (page_id != INVALID_PAGE_ID) {
        if (page_id <= 0 || !visited.insert(page_id).second) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid or cyclic heap page list during DROP",
                          page_id);
        }
        page_id_t next_page_id = INVALID_PAGE_ID;
        {
            ReadPageGuard guard = buffer_pool_.FetchRead(page_id);
            const auto* body = guard.As<std::array<char, PAGE_DATA_SIZE>>();
            next_page_id = SlottedPage(body->data()).GetNextPageId();
        }
        pages.push_back(page_id);
        page_id = next_page_id;
    }
    for (page_id_t page : pages) {
        if (!buffer_pool_.DeletePage(page)) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Could not retire table heap page", page);
        }
    }
    if (!buffer_pool_.DeletePage(header_page_id_)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Could not retire table heap header",
                      header_page_id_);
    }
    header_page_id_ = INVALID_PAGE_ID;
}

}  // namespace relational_db
