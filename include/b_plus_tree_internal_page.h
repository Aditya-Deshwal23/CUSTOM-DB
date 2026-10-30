#ifndef B_PLUS_TREE_INTERNAL_PAGE_H
#define B_PLUS_TREE_INTERNAL_PAGE_H

#include "b_plus_tree_page.h"
#include <utility>
#include <cstring>

namespace relational_db {

#define INTERNAL_PAGE_HEADER_SIZE BPlusTreePage::HEADER_SIZE
#define INTERNAL_PAGE_SIZE \
    ((PAGE_DATA_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (sizeof(std::pair<KeyType, ValueType>)))

/**
 * @brief Internal page does not store actual records, only keys and page IDs representing
 * the child pages.
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
class BPlusTreeInternalPage : public BPlusTreePage {
public:
    // Initialization
    void Init(page_id_t page_id, page_id_t parent_id, int max_size) {
        SetPageType(IndexPageType::INTERNAL_PAGE);
        SetPageId(page_id);
        SetParentPageId(parent_id);
        SetSize(0);
        SetMaxSize(max_size);
    }

    // Key & Value Access
    KeyType KeyAt(int index) const {
        return array_[index].first;
    }

    void SetKeyAt(int index, const KeyType &key) {
        array_[index].first = key;
    }

    ValueType ValueAt(int index) const {
        return array_[index].second;
    }

    void SetValueAt(int index, const ValueType &value) {
        array_[index].second = value;
    }

    int ValueIndex(const ValueType &value) const {
        for (int i = 0; i < GetSize(); ++i) {
            if (array_[i].second == value) {
                return i;
            }
        }
        return -1;
    }

    // Lookup
    ValueType Lookup(const KeyType &key, const KeyComparator &comparator) const {
        // Binary search to find the correct child
        int left = 1; // Start at 1 because index 0 key is invalid (only value matters)
        int right = GetSize() - 1;
        int target = 0;
        
        while (left <= right) {
            int mid = left + (right - left) / 2;
            if (!comparator(key, array_[mid].first)) { // array_[mid].first <= key
                target = mid;
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        }
        return array_[target].second;
    }

    // Insertion
    int InsertNodeAfter(page_id_t old_node_id, const KeyType &new_key, page_id_t new_node_id) {
        int idx = ValueIndex(old_node_id) + 1;
        // Shift elements to make room
        for (int i = GetSize(); i > idx; --i) {
            array_[i] = array_[i - 1];
        }
        array_[idx].first = new_key;
        array_[idx].second = new_node_id;
        IncreaseSize(1);
        return GetSize();
    }

    // Split
    void MoveHalfTo(BPlusTreeInternalPage *recipient) {
        int start_idx = GetMinSize();
        int move_count = GetSize() - start_idx;
        
        // Copy to recipient
        std::memcpy(recipient->array_, array_ + start_idx, move_count * sizeof(std::pair<KeyType, ValueType>));
        
        // Update sizes
        recipient->SetSize(move_count);
        SetSize(start_idx);
    }

private:
    // First key is always invalid, we only use the value (child pointer)
    std::pair<KeyType, ValueType> array_[1]; 
};

} // namespace relational_db

#endif // B_PLUS_TREE_INTERNAL_PAGE_H
