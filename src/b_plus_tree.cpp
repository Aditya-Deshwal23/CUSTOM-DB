#include "b_plus_tree.h"

#include <mutex>
#include <shared_mutex>

namespace relational_db {

template <typename KeyType, typename ValueType, typename KeyComparator>
BPlusTree<KeyType, ValueType, KeyComparator>::BPlusTree(
    std::shared_ptr<BufferPoolManager> bpm, const KeyComparator& cmp)
    : bpm_(std::move(bpm)), comparator_(cmp) {}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::IsEmpty() const {
    return root_page_id_ == INVALID_PAGE_ID;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
WritePageGuard BPlusTree<KeyType, ValueType, KeyComparator>::FindLeafPage(const KeyType& key) {
    if (IsEmpty()) {
        return {};
    }

    WritePageGuard guard = bpm_->FetchWrite(root_page_id_);
    for (;;) {
        const auto* node = guard.As<BPlusTreePage>();
        if (node->IsLeafPage()) {
            return guard;
        }
        const auto* internal = reinterpret_cast<const InternalPage*>(node);
        const page_id_t next_page_id = internal->Lookup(key, comparator_);
        guard.Drop();
        guard = bpm_->FetchWrite(next_page_id);
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
ReadPageGuard BPlusTree<KeyType, ValueType, KeyComparator>::FindLeafPageWithLatchCrabbing(
    const KeyType& key) {
    if (IsEmpty()) {
        return {};
    }

    std::shared_lock<std::shared_mutex> root_read_lock(root_latch_);
    ReadPageGuard current = bpm_->FetchRead(root_page_id_);
    const auto* node = current.As<BPlusTreePage>();
    root_read_lock.unlock();

    while (!node->IsLeafPage()) {
        const auto* internal = reinterpret_cast<const InternalPage*>(node);
        const page_id_t next_page_id = internal->Lookup(key, comparator_);
        ReadPageGuard next = bpm_->FetchRead(next_page_id);
        current.Drop();
        current = std::move(next);
        node = current.As<BPlusTreePage>();
    }
    return current;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::GetValue(
    const KeyType& key, std::vector<ValueType>* result) {
    ReadPageGuard leaf_guard = FindLeafPageWithLatchCrabbing(key);
    if (!leaf_guard) {
        return false;
    }
    const auto* leaf_node = reinterpret_cast<const LeafPage*>(leaf_guard.As<BPlusTreePage>());
    ValueType value;
    const bool found = leaf_node->Lookup(key, value, comparator_);
    if (found) {
        result->push_back(value);
    }
    return found;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Insert(const KeyType& key,
                                                          const ValueType& value) {
    std::unique_lock<std::shared_mutex> lock(root_latch_);
    if (IsEmpty()) {
        StartNewTree(key, value);
        return true;
    }
    return InsertIntoLeaf(key, value);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::StartNewTree(const KeyType& key,
                                                                const ValueType& value) {
    WritePageGuard root_guard = bpm_->NewPageGuarded();
    root_page_id_ = root_guard.GetPageId();
    auto* root_node = reinterpret_cast<LeafPage*>(root_guard.AsMut<BPlusTreePage>());
    root_node->Init(root_page_id_, INVALID_PAGE_ID, LEAF_PAGE_SIZE);
    root_node->Insert(key, value, comparator_);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoLeaf(const KeyType& key,
                                                                  const ValueType& value) {
    WritePageGuard leaf_guard = FindLeafPage(key);
    if (!leaf_guard) {
        return false;
    }
    auto* leaf_node = reinterpret_cast<LeafPage*>(leaf_guard.AsMut<BPlusTreePage>());
    const int size_before = leaf_node->GetSize();
    const int size_after = leaf_node->Insert(key, value, comparator_);
    if (size_after == size_before) {
        return false;
    }
    if (size_after < leaf_node->GetMaxSize()) {
        return true;
    }

    WritePageGuard new_leaf_guard = Split(leaf_node);
    auto* new_leaf_node = reinterpret_cast<LeafPage*>(new_leaf_guard.AsMut<BPlusTreePage>());
    new_leaf_node->SetNextPageId(leaf_node->GetNextPageId());
    leaf_node->SetNextPageId(new_leaf_node->GetPageId());
    const KeyType split_key = new_leaf_node->KeyAt(0);
    InsertIntoParent(leaf_node, split_key, new_leaf_node);
    return true;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
template <typename N>
WritePageGuard BPlusTree<KeyType, ValueType, KeyComparator>::Split(N* node) {
    WritePageGuard new_guard = bpm_->NewPageGuarded();
    auto* new_node = reinterpret_cast<N*>(new_guard.AsMut<BPlusTreePage>());
    new_node->Init(new_guard.GetPageId(), node->GetParentPageId(), node->GetMaxSize());
    node->MoveHalfTo(new_node);
    return new_guard;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoParent(BPlusTreePage* old_node,
                                                                    const KeyType& key,
                                                                    BPlusTreePage* new_node) {
    if (old_node->IsRootPage()) {
        WritePageGuard new_root_guard = bpm_->NewPageGuarded();
        const page_id_t new_root_id = new_root_guard.GetPageId();
        auto* new_root = reinterpret_cast<InternalPage*>(new_root_guard.AsMut<BPlusTreePage>());
        new_root->Init(new_root_id, INVALID_PAGE_ID, INTERNAL_PAGE_SIZE);
        new_root->SetValueAt(0, old_node->GetPageId());
        new_root->SetKeyAt(1, key);
        new_root->SetValueAt(1, new_node->GetPageId());
        new_root->SetSize(2);
        old_node->SetParentPageId(new_root_id);
        new_node->SetParentPageId(new_root_id);
        root_page_id_ = new_root_id;
        return;
    }

    const page_id_t parent_id = old_node->GetParentPageId();
    WritePageGuard parent_guard = bpm_->FetchWrite(parent_id);
    auto* parent_node = reinterpret_cast<InternalPage*>(parent_guard.AsMut<BPlusTreePage>());
    const int new_size = parent_node->InsertNodeAfter(old_node->GetPageId(), key,
                                                       new_node->GetPageId());
    if (new_size < parent_node->GetMaxSize()) {
        return;
    }

    WritePageGuard new_parent_guard = Split(parent_node);
    auto* new_parent_node =
        reinterpret_cast<InternalPage*>(new_parent_guard.AsMut<BPlusTreePage>());
    const KeyType split_key = new_parent_node->KeyAt(0);
    for (int index = 0; index < new_parent_node->GetSize(); ++index) {
        const page_id_t child_id = new_parent_node->ValueAt(index);
        WritePageGuard child_guard = bpm_->FetchWrite(child_id);
        auto* child_node = child_guard.AsMut<BPlusTreePage>();
        child_node->SetParentPageId(new_parent_node->GetPageId());
    }

    InsertIntoParent(parent_node, split_key, new_parent_node);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::Scan(const KeyType& start_key,
                                                        const KeyType& end_key,
                                                        std::vector<ValueType>* result) {
    std::shared_lock<std::shared_mutex> root_lock(root_latch_);
    if (IsEmpty()) {
        return;
    }
    ReadPageGuard leaf_guard = bpm_->FetchRead(root_page_id_);
    const auto* node = leaf_guard.As<BPlusTreePage>();
    while (!node->IsLeafPage()) {
        const auto* internal = reinterpret_cast<const InternalPage*>(node);
        const page_id_t next_page_id = internal->Lookup(start_key, comparator_);
        ReadPageGuard next = bpm_->FetchRead(next_page_id);
        leaf_guard.Drop();
        leaf_guard = std::move(next);
        node = leaf_guard.As<BPlusTreePage>();
    }
    root_lock.unlock();
    if (!leaf_guard) {
        return;
    }

    bool stop = false;
    while (leaf_guard && !stop) {
        const auto* leaf_node = reinterpret_cast<const LeafPage*>(leaf_guard.As<BPlusTreePage>());
        for (int index = 0; index < leaf_node->GetSize(); ++index) {
            if (comparator_(end_key, leaf_node->KeyAt(index))) {
                stop = true;
                break;
            }
            if (!comparator_(leaf_node->KeyAt(index), start_key)) {
                result->push_back(leaf_node->ValueAt(index));
            }
        }

        const page_id_t next_id = leaf_node->GetNextPageId();
        leaf_guard.Drop();
        if (next_id == INVALID_PAGE_ID || stop) {
            break;
        }
        leaf_guard = bpm_->FetchRead(next_id);
    }
}

template class BPlusTree<int, int, std::less<int>>;

}  // namespace relational_db
