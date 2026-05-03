#include "b_plus_tree.h"
#include "page_guard.h"
#include <iostream>
#include <mutex>
#include <shared_mutex>

namespace relational_db {

template <typename KeyType, typename ValueType, typename KeyComparator>
BPlusTree<KeyType, ValueType, KeyComparator>::BPlusTree(std::shared_ptr<BufferPoolManager> bpm, const KeyComparator &cmp)
    : bpm_(std::move(bpm)), comparator_(cmp) {}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::IsEmpty() const {
    return root_page_id_ == INVALID_PAGE_ID;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
Page *BPlusTree<KeyType, ValueType, KeyComparator>::FindLeafPage(const KeyType &key) {
    if (IsEmpty()) return nullptr;
    
    page_id_t curr_page_id = root_page_id_;
    Page *curr_page = bpm_->FetchPage(curr_page_id);
    
    while (curr_page != nullptr) {
        auto *tree_node = reinterpret_cast<BPlusTreePage *>(curr_page->GetData());
        
        if (tree_node->IsLeafPage()) {
            return curr_page; 
        }
        
        auto *internal_node = reinterpret_cast<InternalPage *>(tree_node);
        page_id_t next_page_id = internal_node->Lookup(key, comparator_);
        
        bpm_->UnpinPage(curr_page_id, false);
        curr_page_id = next_page_id;
        curr_page = bpm_->FetchPage(curr_page_id);
    }
    
    return nullptr;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
PageGuard BPlusTree<KeyType, ValueType, KeyComparator>::FindLeafPageWithLatchCrabbing(const KeyType &key) {
    if (IsEmpty()) {
        throw std::runtime_error("Tree is empty");
    }

    // Standard C++17 shared_lock for latch crabbing
    std::shared_lock<std::shared_mutex> root_read_lock(root_latch_);
    page_id_t curr_page_id = root_page_id_;
    
    PageGuard curr_guard = PageGuard::ReadLock(bpm_->FetchPage(curr_page_id));
    auto *curr_node = reinterpret_cast<BPlusTreePage *>(curr_guard.Get()->GetData());
    
    root_read_lock.unlock(); 

    while (!curr_node->IsLeafPage()) {
        auto *internal_node = reinterpret_cast<InternalPage *>(curr_node);
        page_id_t next_page_id = internal_node->Lookup(key, comparator_);

        PageGuard next_guard = PageGuard::ReadLock(bpm_->FetchPage(next_page_id));
        curr_guard.Unlock(); 
        
        curr_guard = std::move(next_guard);
        curr_node = reinterpret_cast<BPlusTreePage *>(curr_guard.Get()->GetData());
    }

    return curr_guard;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::GetValue(const KeyType &key, std::vector<ValueType> *result) {
    try {
        PageGuard leaf_guard = FindLeafPageWithLatchCrabbing(key);
        
        auto *leaf_node = reinterpret_cast<LeafPage *>(leaf_guard.Get()->GetData());
        ValueType val;
        bool found = leaf_node->Lookup(key, val, comparator_);
        
        if (found) {
            result->push_back(val);
        }
        
        return found;
    } catch (...) {
        return false;
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Insert(const KeyType &key, const ValueType &value) {
    std::unique_lock<std::shared_mutex> lock(root_latch_);
    
    if (IsEmpty()) {
        StartNewTree(key, value);
        return true;
    }
    return InsertIntoLeaf(key, value);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::StartNewTree(const KeyType &key, const ValueType &value) {
    page_id_t new_page_id;
    Page *root_page = bpm_->NewPage(&new_page_id);
    if (!root_page) {
        throw std::runtime_error("Out of memory in buffer pool");
    }
    
    root_page_id_ = new_page_id;
    auto *root_node = reinterpret_cast<LeafPage *>(root_page->GetData());
    root_node->Init(root_page_id_, INVALID_PAGE_ID, LEAF_PAGE_SIZE);
    
    root_node->Insert(key, value, comparator_);
    bpm_->UnpinPage(root_page_id_, true);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoLeaf(const KeyType &key, const ValueType &value) {
    Page *leaf_page = FindLeafPage(key);
    if (!leaf_page) return false;
    
    auto *leaf_node = reinterpret_cast<LeafPage *>(leaf_page->GetData());
    int size_before = leaf_node->GetSize();
    int size_after = leaf_node->Insert(key, value, comparator_);
    
    if (size_after == size_before) {
        bpm_->UnpinPage(leaf_page->GetPageId(), false);
        return false;
    }
    
    if (size_after < leaf_node->GetMaxSize()) {
        bpm_->UnpinPage(leaf_page->GetPageId(), true);
        return true;
    }
    
    LeafPage *new_leaf_node = Split(leaf_node);
    new_leaf_node->SetNextPageId(leaf_node->GetNextPageId());
    leaf_node->SetNextPageId(new_leaf_node->GetPageId());
    
    KeyType split_key = new_leaf_node->KeyAt(0);
    InsertIntoParent(leaf_node, split_key, new_leaf_node);
    
    bpm_->UnpinPage(leaf_page->GetPageId(), true);
    bpm_->UnpinPage(new_leaf_node->GetPageId(), true);
    return true;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
template <typename N>
N *BPlusTree<KeyType, ValueType, KeyComparator>::Split(N *node) {
    page_id_t new_page_id;
    Page *new_page = bpm_->NewPage(&new_page_id);
    if (!new_page) {
        throw std::runtime_error("Out of memory during split");
    }
    
    N *new_node = reinterpret_cast<N *>(new_page->GetData());
    new_node->Init(new_page_id, node->GetParentPageId(), node->GetMaxSize());
    
    node->MoveHalfTo(new_node);
    return new_node;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoParent(BPlusTreePage *old_node, const KeyType &key, BPlusTreePage *new_node) {
    if (old_node->IsRootPage()) {
        page_id_t new_root_id;
        Page *new_root_page = bpm_->NewPage(&new_root_id);
        if (!new_root_page) {
            throw std::runtime_error("Out of memory creating new root");
        }
        
        auto *new_root = reinterpret_cast<InternalPage *>(new_root_page->GetData());
        new_root->Init(new_root_id, INVALID_PAGE_ID, INTERNAL_PAGE_SIZE);
        
        new_root->SetValueAt(0, old_node->GetPageId()); 
        new_root->SetKeyAt(1, key);
        new_root->SetValueAt(1, new_node->GetPageId());
        new_root->SetSize(2);
        
        old_node->SetParentPageId(new_root_id);
        new_node->SetParentPageId(new_root_id);
        
        root_page_id_ = new_root_id;
        bpm_->UnpinPage(new_root_id, true);
        return;
    }
    
    page_id_t parent_id = old_node->GetParentPageId();
    Page *parent_page = bpm_->FetchPage(parent_id);
    auto *parent_node = reinterpret_cast<InternalPage *>(parent_page->GetData());
    
    int new_size = parent_node->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
    
    if (new_size < parent_node->GetMaxSize()) {
        bpm_->UnpinPage(parent_id, true);
        return;
    }
    
    InternalPage *new_parent_node = Split(parent_node);
    KeyType split_key = new_parent_node->KeyAt(0); 
    
    for (int i = 0; i < new_parent_node->GetSize(); ++i) {
        page_id_t child_id = new_parent_node->ValueAt(i);
        Page *child_page = bpm_->FetchPage(child_id);
        auto *child_node = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
        child_node->SetParentPageId(new_parent_node->GetPageId());
        bpm_->UnpinPage(child_id, true);
    }
    
    InsertIntoParent(parent_node, split_key, new_parent_node);
    
    bpm_->UnpinPage(parent_id, true);
    bpm_->UnpinPage(new_parent_node->GetPageId(), true);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::Scan(const KeyType &start_key, const KeyType &end_key, std::vector<ValueType> *result) {
    std::shared_lock<std::shared_mutex> lock(root_latch_);
    Page *leaf_page = FindLeafPage(start_key);
    if (!leaf_page) return;
    
    auto *leaf_node = reinterpret_cast<LeafPage *>(leaf_page->GetData());
    bool stop = false;
    
    while (leaf_node != nullptr && !stop) {
        for (int i = 0; i < leaf_node->GetSize(); ++i) {
            if (comparator_(end_key, leaf_node->KeyAt(i))) {
                stop = true;
                break;
            }
            if (!comparator_(leaf_node->KeyAt(i), start_key)) {
                result->push_back(leaf_node->ValueAt(i));
            }
        }
        
        page_id_t next_id = leaf_node->GetNextPageId();
        bpm_->UnpinPage(leaf_page->GetPageId(), false);
        
        if (next_id == INVALID_PAGE_ID || stop) {
            break;
        }
        
        leaf_page = bpm_->FetchPage(next_id);
        leaf_node = reinterpret_cast<LeafPage *>(leaf_page->GetData());
    }
}

template class BPlusTree<int, int, std::less<int>>;

} // namespace relational_db
