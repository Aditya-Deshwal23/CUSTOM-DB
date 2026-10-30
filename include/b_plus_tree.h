#ifndef B_PLUS_TREE_H
#define B_PLUS_TREE_H

#include "buffer_pool_manager.h"
#include "b_plus_tree_internal_page.h"
#include "b_plus_tree_leaf_page.h"
#include "page_guard.h"
#include <vector>
#include <memory>
#include <shared_mutex>

namespace relational_db {

/**
 * @brief B+ Tree Index implementation.
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
class BPlusTree {
    using InternalPage = BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>;
    using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>;

public:
    explicit BPlusTree(std::shared_ptr<BufferPoolManager> bpm, const KeyComparator &cmp);

    bool IsEmpty() const;
    bool GetValue(const KeyType &key, std::vector<ValueType> *result);
    bool Insert(const KeyType &key, const ValueType &value);
    void Scan(const KeyType &start_key, const KeyType &end_key, std::vector<ValueType> *result);

private:
    std::shared_ptr<BufferPoolManager> bpm_;
    KeyComparator comparator_;
    page_id_t root_page_id_{INVALID_PAGE_ID};
    
    // Explicit standard shared_mutex
    std::shared_mutex root_latch_;

    ReadPageGuard FindLeafPageWithLatchCrabbing(const KeyType &key);
    WritePageGuard FindLeafPage(const KeyType &key);

    void StartNewTree(const KeyType &key, const ValueType &value);
    bool InsertIntoLeaf(const KeyType &key, const ValueType &value);
    void InsertIntoParent(BPlusTreePage *old_node, const KeyType &key, BPlusTreePage *new_node);

    template <typename N>
    WritePageGuard Split(N *node);
};

} // namespace relational_db

#endif // B_PLUS_TREE_H
