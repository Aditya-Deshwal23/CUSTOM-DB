#ifndef B_PLUS_TREE_PAGE_H
#define B_PLUS_TREE_PAGE_H

#include "config.h"
#include <cstdint>

namespace relational_db {

/**
 * Enum for B+ tree page types
 */
enum class IndexPageType { INVALID_INDEX_PAGE = 0, LEAF_PAGE, INTERNAL_PAGE };

/**
 * @brief Base class for B+ tree pages. 
 * Provides a wrapper around the raw page data buffer to access common header fields.
 * 
 * Header layout:
 * - PageType (4 bytes)
 * - CurrentSize (4 bytes)
 * - MaxSize (4 bytes)
 * - ParentPageId (4 bytes)
 * - PageId (4 bytes)
 */
class BPlusTreePage {
public:
    static constexpr size_t HEADER_SIZE = 20;

    bool IsLeafPage() const { return page_type_ == IndexPageType::LEAF_PAGE; }
    bool IsInternalPage() const { return page_type_ == IndexPageType::INTERNAL_PAGE; }
    bool IsRootPage() const { return parent_page_id_ == INVALID_PAGE_ID; }
    
    void SetPageType(IndexPageType page_type) { page_type_ = page_type; }

    int GetSize() const { return size_; }
    void SetSize(int size) { size_ = size; }
    void IncreaseSize(int amount) { size_ += amount; }

    int GetMaxSize() const { return max_size_; }
    void SetMaxSize(int max_size) { max_size_ = max_size; }

    int GetMinSize() const { return IsRootPage() ? (IsLeafPage() ? 1 : 2) : max_size_ / 2; }

    page_id_t GetParentPageId() const { return parent_page_id_; }
    void SetParentPageId(page_id_t parent_page_id) { parent_page_id_ = parent_page_id; }

    page_id_t GetPageId() const { return page_id_; }
    void SetPageId(page_id_t page_id) { page_id_ = page_id; }

private:
    IndexPageType page_type_;
    int size_;
    int max_size_;
    page_id_t parent_page_id_;
    page_id_t page_id_;
};

} // namespace relational_db

#endif // B_PLUS_TREE_PAGE_H
