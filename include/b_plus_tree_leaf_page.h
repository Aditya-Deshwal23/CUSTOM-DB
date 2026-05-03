#ifndef B_PLUS_TREE_LEAF_PAGE_H
#define B_PLUS_TREE_LEAF_PAGE_H

#include "b_plus_tree_page.h"
#include <utility>
#include <cstring>

namespace relational_db {

#define LEAF_PAGE_HEADER_SIZE (BPlusTreePage::HEADER_SIZE + sizeof(page_id_t))
#define LEAF_PAGE_SIZE ((PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (sizeof(std::pair<KeyType, ValueType>)))

/**
 * @brief Leaf page stores the actual data (or record IDs).
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
class BPlusTreeLeafPage : public BPlusTreePage {
public:
    void Init(page_id_t page_id, page_id_t parent_id, int max_size) {
        SetPageType(IndexPageType::LEAF_PAGE);
        SetPageId(page_id);
        SetParentPageId(parent_id);
        SetSize(0);
        SetMaxSize(max_size);
        next_page_id_ = INVALID_PAGE_ID;
    }

    page_id_t GetNextPageId() const { return next_page_id_; }
    void SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

    KeyType KeyAt(int index) const { return array_[index].first; }
    ValueType ValueAt(int index) const { return array_[index].second; }

    // Lookup
    bool Lookup(const KeyType &key, ValueType &value, const KeyComparator &comparator) const {
        for (int i = 0; i < GetSize(); ++i) {
            if (!comparator(array_[i].first, key) && !comparator(key, array_[i].first)) {
                value = array_[i].second;
                return true;
            }
        }
        return false;
    }

    // Insert
    int Insert(const KeyType &key, const ValueType &value, const KeyComparator &comparator) {
        int idx = 0;
        while (idx < GetSize() && comparator(array_[idx].first, key)) {
            idx++;
        }

        if (idx < GetSize() && !comparator(array_[idx].first, key) && !comparator(key, array_[idx].first)) {
            return GetSize(); // Key already exists
        }

        // Shift elements
        for (int i = GetSize(); i > idx; --i) {
            array_[i] = array_[i - 1];
        }

        array_[idx].first = key;
        array_[idx].second = value;
        IncreaseSize(1);
        return GetSize();
    }

    // Split
    void MoveHalfTo(BPlusTreeLeafPage *recipient) {
        int start_idx = GetMinSize();
        int move_count = GetSize() - start_idx;
        
        std::memcpy(recipient->array_, array_ + start_idx, move_count * sizeof(std::pair<KeyType, ValueType>));
        
        recipient->SetSize(move_count);
        SetSize(start_idx);
        
        // Link leaves
        recipient->SetNextPageId(GetNextPageId());
        SetNextPageId(recipient->GetPageId());
    }

private:
    page_id_t next_page_id_;
    std::pair<KeyType, ValueType> array_[1];
};

} // namespace relational_db

#endif // B_PLUS_TREE_LEAF_PAGE_H
