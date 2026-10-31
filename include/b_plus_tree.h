#ifndef B_PLUS_TREE_H
#define B_PLUS_TREE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "buffer_pool_manager.h"
#include "index_key.h"

namespace relational_db {
namespace bpt_detail {

template <typename Key, typename Enable = void>
struct KeyCodec;

template <typename Key>
struct KeyCodec<Key, typename std::enable_if<std::is_integral<Key>::value>::type> {
    using Unsigned = typename std::make_unsigned<Key>::type;
    static constexpr size_t kSize = sizeof(Key);

    static std::array<uint8_t, kSize> Encode(Key value) noexcept {
        Unsigned ordered = static_cast<Unsigned>(value);
        if constexpr (std::is_signed<Key>::value) {
            ordered ^= Unsigned{1} << (kSize * 8U - 1U);
        }
        std::array<uint8_t, kSize> bytes{};
        for (size_t index = 0; index < kSize; ++index) {
            bytes[index] = static_cast<uint8_t>(ordered >> ((kSize - index - 1U) * 8U));
        }
        return bytes;
    }

    static Key Decode(const uint8_t* bytes) noexcept {
        Unsigned ordered = 0;
        for (size_t index = 0; index < kSize; ++index) {
            ordered = static_cast<Unsigned>((ordered << 8U) | bytes[index]);
        }
        if constexpr (std::is_signed<Key>::value) {
            ordered ^= Unsigned{1} << (kSize * 8U - 1U);
            Key value{};
            std::memcpy(&value, &ordered, sizeof(value));
            return value;
        }
        return static_cast<Key>(ordered);
    }
};

template <>
struct KeyCodec<bool, void> {
    static constexpr size_t kSize = 1;
    static std::array<uint8_t, kSize> Encode(bool value) noexcept {
        return {static_cast<uint8_t>(value ? 1U : 0U)};
    }
    static bool Decode(const uint8_t* bytes) noexcept {
        return bytes[0] != 0;
    }
};

template <>
struct KeyCodec<IndexKey16, void> {
    static constexpr size_t kSize = 16;

    static std::array<uint8_t, kSize> Encode(const IndexKey16& key) noexcept {
        return key.bytes;
    }
    static IndexKey16 Decode(const uint8_t* bytes) noexcept {
        IndexKey16 key;
        std::copy_n(bytes, kSize, key.bytes.begin());
        return key;
    }
};

template <typename Value>
struct ValueCodec {
    static_assert(std::is_integral<Value>::value && sizeof(Value) == 8,
                  "BPlusTree values must be 8-byte integral RID values");
    using Unsigned = typename std::make_unsigned<Value>::type;

    static std::array<uint8_t, 8> Encode(Value value) noexcept {
        const Unsigned bits = static_cast<Unsigned>(value);
        std::array<uint8_t, 8> bytes{};
        for (size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<uint8_t>(bits >> (index * 8U));
        }
        return bytes;
    }

    static Value Decode(const uint8_t* bytes) noexcept {
        Unsigned bits = 0;
        for (size_t index = 0; index < 8; ++index) {
            bits |= static_cast<Unsigned>(bytes[index]) << (index * 8U);
        }
        if constexpr (std::is_signed<Value>::value) {
            if (bits > static_cast<Unsigned>(std::numeric_limits<Value>::max())) {
                return static_cast<Value>(-1 - static_cast<Value>(static_cast<Unsigned>(~bits)));
            }
        }
        return static_cast<Value>(bits);
    }
};

template <size_t KeySize, size_t Capacity>
struct LeafPageLayout {
    struct Entry {
        std::array<uint8_t, KeySize> key;
        std::array<uint8_t, 8> value;
    };
    std::array<uint8_t, 2> count;
    std::array<uint8_t, 4> next_page_id;
    std::array<Entry, Capacity> entries;
};

template <size_t KeySize, size_t ChildCapacity>
struct InternalPageLayout {
    struct Entry {
        std::array<uint8_t, KeySize> key;
        std::array<uint8_t, 4> child_page_id;
    };
    std::array<uint8_t, 2> count;
    std::array<Entry, ChildCapacity> entries;
};

struct IndexMetaLayout {
    std::array<uint8_t, 4> root_page_id;
    std::array<uint8_t, 2> height;
    std::array<uint8_t, 2> key_kind;
};

static_assert(sizeof(IndexMetaLayout) == 8, "INDEX_META body layout changed");
static_assert(offsetof(IndexMetaLayout, root_page_id) == 0, "INDEX_META root offset changed");
static_assert(offsetof(IndexMetaLayout, height) == 4, "INDEX_META height offset changed");
static_assert(offsetof(IndexMetaLayout, key_kind) == 6, "INDEX_META key kind offset changed");

inline uint16_t ReadU16(const uint8_t* bytes) noexcept {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

inline uint32_t ReadU32(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

inline void WriteU16(uint8_t* bytes, uint16_t value) noexcept {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8U);
}

inline void WriteU32(uint8_t* bytes, uint32_t value) noexcept {
    for (size_t index = 0; index < 4; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

inline int32_t ReadPageId(const uint8_t* bytes) noexcept {
    return static_cast<int32_t>(ReadU32(bytes));
}

inline void WritePageId(uint8_t* bytes, page_id_t page_id) noexcept {
    WriteU32(bytes, static_cast<uint32_t>(page_id));
}

}  // namespace bpt_detail

template <typename KeyType, typename ValueType, typename KeyComparator>
class BPlusTree {
    using KeyCodec = bpt_detail::KeyCodec<KeyType>;
    using ValueCodec = bpt_detail::ValueCodec<ValueType>;
    static constexpr size_t kKeySize = KeyCodec::kSize;
    static constexpr size_t kLeafCapacity = (PAGE_DATA_SIZE - 6U) / (kKeySize + 8U);
    static constexpr size_t kInternalChildCapacity =
        (PAGE_DATA_SIZE - 2U) / (kKeySize + sizeof(page_id_t));
    static constexpr size_t kInternalKeyCapacity = kInternalChildCapacity - 1U;
    using LeafLayout = bpt_detail::LeafPageLayout<kKeySize, kLeafCapacity>;
    using InternalLayout =
        bpt_detail::InternalPageLayout<kKeySize, kInternalChildCapacity>;
    using LeafEntry = typename LeafLayout::Entry;
    using InternalEntry = typename InternalLayout::Entry;
    using BytePage = std::array<char, PAGE_DATA_SIZE>;

    static_assert(kKeySize > 0 && kKeySize <= 16, "BPlusTree key encoding must fit 16 bytes");
    static_assert(std::is_same<KeyType, IndexKey16>::value ||
                      std::is_same<KeyType, bool>::value ||
                      (std::is_integral<KeyType>::value &&
                       (sizeof(KeyType) == 4 || sizeof(KeyType) == 8)),
                  "BPlusTree keys must be INT32, INT64, BOOLEAN, or IndexKey16");
    static_assert(kLeafCapacity > 1 && kLeafCapacity <= std::numeric_limits<uint16_t>::max(),
                  "BPlusTree leaf capacity is invalid");
    static_assert(kInternalChildCapacity > 2 &&
                      kInternalChildCapacity <= std::numeric_limits<uint16_t>::max(),
                  "BPlusTree internal capacity is invalid");
    static_assert(sizeof(LeafLayout) <= PAGE_DATA_SIZE, "Leaf layout exceeds page body");
    static_assert(sizeof(InternalLayout) <= PAGE_DATA_SIZE, "Internal layout exceeds page body");
    static_assert(offsetof(LeafLayout, count) == 0, "Leaf count offset changed");
    static_assert(offsetof(LeafLayout, next_page_id) == 2, "Leaf link offset changed");
    static_assert(offsetof(LeafLayout, entries) == 6, "Leaf entries offset changed");
    static_assert(offsetof(InternalLayout, count) == 0, "Internal count offset changed");
    static_assert(offsetof(InternalLayout, entries) == 2, "Internal entries offset changed");
    static_assert(sizeof(LeafEntry) == kKeySize + 8U, "Leaf entry has padding");
    static_assert(sizeof(InternalEntry) == kKeySize + sizeof(page_id_t),
                  "Internal entry has padding");
    static_assert(std::is_standard_layout<LeafLayout>::value &&
                      std::is_trivially_copyable<LeafLayout>::value,
                  "Leaf page layout must be a plain byte layout");
    static_assert(std::is_standard_layout<InternalLayout>::value &&
                      std::is_trivially_copyable<InternalLayout>::value,
                  "Internal page layout must be a plain byte layout");

    struct DescentPath {
        std::vector<page_id_t> pages;
        std::vector<size_t> child_indexes;
    };

    struct AuditRange {
        std::optional<KeyType> minimum;
        std::optional<KeyType> maximum;
    };

public:
    static constexpr IndexKeyKind DefaultKeyKind() {
        if constexpr (std::is_same<KeyType, bool>::value) {
            return IndexKeyKind::BOOLEAN;
        } else if constexpr (std::is_integral<KeyType>::value && sizeof(KeyType) == 4) {
            return IndexKeyKind::INT32;
        } else if constexpr (std::is_integral<KeyType>::value && sizeof(KeyType) == 8) {
            return IndexKeyKind::INT64;
        }
        return IndexKeyKind::RAW_INTEGRAL;
    }

    class Iterator {
    public:
        Iterator() = default;
        Iterator(Iterator&&) noexcept = default;
        Iterator& operator=(Iterator&&) noexcept = default;
        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;

        std::optional<std::pair<KeyType, ValueType>> Next() {
            while (leaf_guard_) {
                const auto* leaf = LeafRead(leaf_guard_);
                const uint16_t count = ReadCount(leaf->count.data());
                if (index_ < count) {
                    const LeafEntry& entry = leaf->entries[index_++];
                    return std::make_pair(KeyCodec::Decode(entry.key.data()),
                                          ValueCodec::Decode(entry.value.data()));
                }
                const page_id_t next = bpt_detail::ReadPageId(leaf->next_page_id.data());
                leaf_guard_.Drop();
                if (next == INVALID_PAGE_ID) {
                    break;
                }
                leaf_guard_ = FetchRead(*buffer_pool_, next);
                index_ = 0;
            }
            return std::nullopt;
        }

    private:
        friend class BPlusTree;
        Iterator(std::shared_ptr<BufferPoolManager> buffer_pool, ReadPageGuard guard,
                 uint16_t index)
            : buffer_pool_(std::move(buffer_pool)), leaf_guard_(std::move(guard)), index_(index) {}

        static const LeafLayout* LeafRead(const ReadPageGuard& guard) {
            if (guard.GetPageType() != PageType::BTREE_LEAF) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Expected B+ tree leaf page",
                              guard.GetPageId());
            }
            const BytePage* page = guard.As<BytePage>();
            return reinterpret_cast<const LeafLayout*>(page->data());
        }

        static ReadPageGuard FetchRead(BufferPoolManager& bpm, page_id_t page_id) {
            ReadPageGuard guard = bpm.FetchRead(page_id);
            if (!guard) {
                throw DbError(ErrorCode::PAGE_OUT_OF_RANGE, "Failed to fetch B+ tree leaf",
                              page_id);
            }
            return guard;
        }

        std::shared_ptr<BufferPoolManager> buffer_pool_;
        ReadPageGuard leaf_guard_;
        uint16_t index_{0};
    };

    explicit BPlusTree(std::shared_ptr<BufferPoolManager> buffer_pool,
                       const KeyComparator& comparator,
                       IndexKeyKind key_kind = DefaultKeyKind())
        : buffer_pool_(std::move(buffer_pool)), comparator_(comparator), key_kind_(key_kind) {
        InitializeCatalogMeta();
    }

    BPlusTree(std::shared_ptr<BufferPoolManager> buffer_pool, const KeyComparator& comparator,
              page_id_t index_meta_page_id, IndexKeyKind key_kind)
        : buffer_pool_(std::move(buffer_pool)),
          comparator_(comparator),
          key_kind_(key_kind),
          meta_page_id_(index_meta_page_id) {
        if (!buffer_pool_ || meta_page_id_ <= 0) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Invalid B+ tree metadata page");
        }
        LoadMeta();
    }

    bool IsEmpty() const noexcept {
        return root_page_id_ == INVALID_PAGE_ID;
    }

    uint16_t Height() const noexcept {
        return height_;
    }

    page_id_t MetaPageId() const noexcept {
        return meta_page_id_;
    }

    void Drop() {
        std::vector<page_id_t> pending;
        std::vector<page_id_t> pages;
        std::unordered_set<page_id_t> visited;
        if (root_page_id_ != INVALID_PAGE_ID) {
            pending.push_back(root_page_id_);
        }
        while (!pending.empty()) {
            const page_id_t page_id = pending.back();
            pending.pop_back();
            if (!visited.insert(page_id).second) {
                continue;
            }
            pages.push_back(page_id);
            ReadPageGuard guard = FetchRead(page_id);
            if (guard.GetPageType() == PageType::BTREE_INTERNAL) {
                const auto* internal = InternalRead(guard);
                const uint16_t count = ReadCount(internal->count.data());
                if (count > kInternalKeyCapacity) {
                    throw DbError(ErrorCode::PAGE_CORRUPT, "Invalid B+ tree node during DROP",
                                  page_id);
                }
                for (uint16_t index = 0; index <= count; ++index) {
                    pending.push_back(
                        bpt_detail::ReadPageId(internal->entries[index].child_page_id.data()));
                }
            } else if (guard.GetPageType() != PageType::BTREE_LEAF) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Unexpected page in B+ tree during DROP",
                              page_id);
            }
        }
        for (page_id_t page_id : pages) {
            if (!buffer_pool_->DeletePage(page_id)) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Could not retire B+ tree page", page_id);
            }
        }
        if (!buffer_pool_->DeletePage(meta_page_id_)) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Could not retire B+ tree metadata",
                          meta_page_id_);
        }
        root_page_id_ = INVALID_PAGE_ID;
        height_ = 0;
        meta_page_id_ = INVALID_PAGE_ID;
    }

    double FillFactor() const {
        if (IsEmpty()) {
            return 0.0;
        }
        page_id_t page_id = root_page_id_;
        for (uint16_t level = height_; level > 1; --level) {
            ReadPageGuard guard = FetchRead(page_id);
            const auto* internal = InternalRead(guard);
            page_id = bpt_detail::ReadPageId(internal->entries[0].child_page_id.data());
        }
        uint64_t occupied = 0;
        uint64_t capacity = 0;
        while (page_id != INVALID_PAGE_ID) {
            ReadPageGuard guard = FetchRead(page_id);
            const auto* leaf = LeafRead(guard);
            occupied += ReadCount(leaf->count.data());
            capacity += kLeafCapacity;
            page_id = bpt_detail::ReadPageId(leaf->next_page_id.data());
        }
        return capacity == 0 ? 0.0 : static_cast<double>(occupied) / capacity;
    }

    std::optional<ValueType> GetValue(const KeyType& key) const {
        if (IsEmpty()) {
            return std::nullopt;
        }
        DescentPath path;
        const page_id_t leaf_id = FindLeaf(key, &path);
        ReadPageGuard guard = FetchRead(leaf_id);
        const auto* leaf = LeafRead(guard);
        const uint16_t count = ReadCount(leaf->count.data());
        const size_t index = LeafLowerBound(leaf, count, key);
        if (index == count || !KeysEqual(DecodeKey(leaf->entries[index]), key)) {
            return std::nullopt;
        }
        return ValueCodec::Decode(leaf->entries[index].value.data());
    }

    bool Insert(const KeyType& key, const ValueType& value) {
        if (IsEmpty()) {
            last_inserted_key_ = key;
            WritePageGuard leaf_guard = NewPage();
            const page_id_t new_root = leaf_guard.GetPageId();
            leaf_guard.SetPageType(PageType::BTREE_LEAF);
            auto* leaf = LeafWrite(leaf_guard);
            InitializeLeaf(leaf, INVALID_PAGE_ID);
            WriteLeafEntry(leaf->entries[0], key, value);
            WriteCount(leaf->count.data(), 1);
            leaf_guard.Drop();
            SetRoot(new_root, 1);
            return true;
        }

        DescentPath path;
        const page_id_t leaf_id = FindLeaf(key, &path);
        WritePageGuard leaf_guard = FetchWrite(leaf_id);
        auto* leaf = LeafWrite(leaf_guard);
        const uint16_t old_count = ReadCount(leaf->count.data());
        const size_t index = LeafLowerBound(leaf, old_count, key);
        if (index < old_count && KeysEqual(DecodeKey(leaf->entries[index]), key)) {
            throw DbError(ErrorCode::DUPLICATE_KEY, "Duplicate key in unique B+ tree index");
        }
        if (last_inserted_key_ && !comparator_(*last_inserted_key_, key)) {
            ascending_inserts_ = false;
        }
        last_inserted_key_ = key;

        if (old_count < kLeafCapacity) {
            std::move_backward(leaf->entries.begin() + static_cast<std::ptrdiff_t>(index),
                               leaf->entries.begin() + old_count,
                               leaf->entries.begin() + old_count + 1U);
            WriteLeafEntry(leaf->entries[index], key, value);
            WriteCount(leaf->count.data(), static_cast<uint16_t>(old_count + 1U));
            return true;
        }

        std::vector<LeafEntry> entries(leaf->entries.begin(),
                                       leaf->entries.begin() + old_count);
        LeafEntry inserted{};
        WriteLeafEntry(inserted, key, value);
        entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(index), inserted);

        WritePageGuard right_guard = NewPage();
        const page_id_t right_id = right_guard.GetPageId();
        right_guard.SetPageType(PageType::BTREE_LEAF);
        auto* right = LeafWrite(right_guard);
        const page_id_t old_next = bpt_detail::ReadPageId(leaf->next_page_id.data());
        InitializeLeaf(right, old_next);

        const bool append_to_rightmost =
            ascending_inserts_ && index == old_count && old_next == INVALID_PAGE_ID;
        size_t left_count = append_to_rightmost ? (kLeafCapacity * 9U) / 10U
                                               : entries.size() / 2U;
        left_count = std::max(left_count, static_cast<size_t>(LeafMinimum()));
        left_count = std::min(left_count, entries.size() - LeafMinimum());
        std::copy_n(entries.begin(), left_count, leaf->entries.begin());
        std::copy(entries.begin() + static_cast<std::ptrdiff_t>(left_count), entries.end(),
                  right->entries.begin());
        WriteCount(leaf->count.data(), static_cast<uint16_t>(left_count));
        WriteCount(right->count.data(),
                   static_cast<uint16_t>(entries.size() - left_count));
        bpt_detail::WritePageId(leaf->next_page_id.data(), right_id);
        const KeyType separator = DecodeKey(right->entries[0]);
        right_guard.Drop();
        leaf_guard.Drop();
        InsertIntoParent(path, separator, right_id);
        return true;
    }

    bool Delete(const KeyType& key) {
        if (IsEmpty()) {
            return false;
        }
        DescentPath path;
        const page_id_t leaf_id = FindLeaf(key, &path);
        WritePageGuard leaf_guard = FetchWrite(leaf_id);
        auto* leaf = LeafWrite(leaf_guard);
        const uint16_t old_count = ReadCount(leaf->count.data());
        const size_t index = LeafLowerBound(leaf, old_count, key);
        if (index == old_count || !KeysEqual(DecodeKey(leaf->entries[index]), key)) {
            return false;
        }
        ascending_inserts_ = false;
        std::move(leaf->entries.begin() + static_cast<std::ptrdiff_t>(index + 1U),
                  leaf->entries.begin() + old_count,
                  leaf->entries.begin() + static_cast<std::ptrdiff_t>(index));
        const uint16_t new_count = static_cast<uint16_t>(old_count - 1U);
        WriteCount(leaf->count.data(), new_count);

        if (path.pages.size() == 1) {
            leaf_guard.Drop();
            if (new_count == 0) {
                if (!buffer_pool_->DeletePage(leaf_id)) {
                    throw DbError(ErrorCode::PAGE_CORRUPT, "Could not retire empty root leaf",
                                  leaf_id);
                }
                SetRoot(INVALID_PAGE_ID, 0);
            }
            return true;
        }

        std::optional<KeyType> new_minimum;
        if (new_count != 0) {
            new_minimum = DecodeKey(leaf->entries[0]);
        }
        leaf_guard.Drop();
        if (new_minimum) {
            FixSeparator(path, *new_minimum);
        }
        if (new_count < LeafMinimum()) {
            RebalanceLeaf(path);
        }
        return true;
    }

    Iterator Begin() const {
        if (IsEmpty()) {
            return End();
        }
        page_id_t page_id = root_page_id_;
        for (uint16_t level = height_; level > 1; --level) {
            ReadPageGuard guard = FetchRead(page_id);
            const auto* internal = InternalRead(guard);
            page_id = bpt_detail::ReadPageId(internal->entries[0].child_page_id.data());
        }
        return Iterator(buffer_pool_, FetchRead(page_id), 0);
    }

    Iterator End() const {
        return Iterator{};
    }

    Iterator LowerBound(const KeyType& key) const {
        if (IsEmpty()) {
            return End();
        }
        const page_id_t leaf_id = FindLeaf(key, nullptr);
        ReadPageGuard guard = FetchRead(leaf_id);
        const auto* leaf = LeafRead(guard);
        return Iterator(buffer_pool_, std::move(guard),
                        static_cast<uint16_t>(
                            LeafLowerBound(leaf, ReadCount(leaf->count.data()), key)));
    }

    std::vector<std::pair<KeyType, ValueType>> RangeScan(
        const KeyType& low, const KeyType& high, bool include_low = true,
        bool include_high = false) const {
        std::vector<std::pair<KeyType, ValueType>> result;
        if (comparator_(high, low) ||
            (KeysEqual(low, high) && (!include_low || !include_high))) {
            return result;
        }
        Iterator iterator = LowerBound(low);
        while (std::optional<std::pair<KeyType, ValueType>> item = iterator.Next()) {
            if (comparator_(item->first, low) || (!include_low && KeysEqual(item->first, low))) {
                continue;
            }
            if (comparator_(high, item->first) ||
                (!include_high && KeysEqual(item->first, high))) {
                break;
            }
            result.push_back(*item);
        }
        return result;
    }

    std::optional<std::string> Validate() const {
        try {
        if (meta_page_id_ <= 0) {
            return "INDEX_META page ID is invalid";
        }
        if (catalog_root_meta_) {
            ReadPageGuard header_guard = FetchRead(0);
            const auto* header = header_guard.As<BytePage>();
            const page_id_t catalog_root = bpt_detail::ReadPageId(
                reinterpret_cast<const uint8_t*>(header->data() + 28));
            if (catalog_root != meta_page_id_) {
                return "Database header catalog_root does not reference INDEX_META";
            }
        }
        if ((root_page_id_ == INVALID_PAGE_ID) != (height_ == 0)) {
            return "INDEX_META root and height disagree";
        }
        bpt_detail::IndexMetaLayout stored{};
        {
            ReadPageGuard guard = FetchRead(meta_page_id_);
            std::memcpy(&stored, guard.As<BytePage>()->data(), sizeof(stored));
        }
        if (bpt_detail::ReadPageId(stored.root_page_id.data()) != root_page_id_ ||
            bpt_detail::ReadU16(stored.height.data()) != height_ ||
            bpt_detail::ReadU16(stored.key_kind.data()) !=
                static_cast<uint16_t>(key_kind_)) {
            return "INDEX_META does not match the in-memory tree state";
        }
        if (IsEmpty()) {
            return std::nullopt;
        }

        std::vector<page_id_t> leaves;
        std::vector<page_id_t> recursion_stack;
        AuditRange range;
        std::optional<std::string> error;
        std::function<AuditRange(page_id_t, uint16_t, bool)> audit =
            [&](page_id_t page_id, uint16_t level, bool is_root) -> AuditRange {
            if (error) {
                return {};
            }
            if (std::find(recursion_stack.begin(), recursion_stack.end(), page_id) !=
                recursion_stack.end()) {
                error = "B+ tree child pointers contain a cycle";
                return {};
            }
            recursion_stack.push_back(page_id);
            AuditRange result;
            if (level == 1) {
                ReadPageGuard guard = FetchRead(page_id);
                const auto* leaf = LeafRead(guard);
                const uint16_t count = ReadCount(leaf->count.data());
                if (count > kLeafCapacity || (!is_root && count < LeafMinimum()) ||
                    (is_root && count == 0)) {
                    error = "Leaf occupancy is invalid";
                } else {
                    for (uint16_t index = 0; index < count; ++index) {
                        const KeyType current = DecodeKey(leaf->entries[index]);
                        if (index != 0 &&
                            !comparator_(DecodeKey(leaf->entries[index - 1]), current)) {
                            error = "Leaf keys are not strictly ordered";
                            break;
                        }
                    }
                    if (count != 0) {
                        result.minimum = DecodeKey(leaf->entries[0]);
                        result.maximum = DecodeKey(leaf->entries[count - 1U]);
                    }
                }
                leaves.push_back(page_id);
            } else {
                ReadPageGuard guard = FetchRead(page_id);
                const auto* internal = InternalRead(guard);
                const uint16_t count = ReadCount(internal->count.data());
                if (count > kInternalKeyCapacity ||
                    (!is_root && count < InternalMinimumKeys()) ||
                    (is_root && count == 0)) {
                    error = "Internal-node occupancy is invalid";
                }
                for (uint16_t child_index = 0;
                     !error && child_index <= count; ++child_index) {
                    const page_id_t child =
                        bpt_detail::ReadPageId(internal->entries[child_index].child_page_id.data());
                    AuditRange child_range = audit(child, static_cast<uint16_t>(level - 1U), false);
                    if (error) {
                        break;
                    }
                    if (child_index == 0) {
                        result.minimum = child_range.minimum;
                    } else {
                        const KeyType separator =
                            DecodeKey(internal->entries[child_index]);
                        if (!child_range.minimum ||
                            !KeysEqual(separator, *child_range.minimum) ||
                            !result.maximum || !comparator_(*result.maximum, separator)) {
                            error = "Internal separator bounds are invalid";
                            break;
                        }
                    }
                    if (child_range.maximum) {
                        result.maximum = child_range.maximum;
                    }
                }
            }
            recursion_stack.pop_back();
            return result;
        };
        range = audit(root_page_id_, height_, true);
        (void)range;
        if (error) {
            return error;
        }

        std::vector<page_id_t> chain;
        std::vector<KeyType> chain_keys;
        page_id_t page_id = leaves.front();
        std::vector<page_id_t> seen;
        while (page_id != INVALID_PAGE_ID) {
            if (std::find(seen.begin(), seen.end(), page_id) != seen.end()) {
                return "Leaf chain contains a cycle";
            }
            seen.push_back(page_id);
            chain.push_back(page_id);
            ReadPageGuard guard = FetchRead(page_id);
            const auto* leaf = LeafRead(guard);
            const uint16_t count = ReadCount(leaf->count.data());
            for (uint16_t index = 0; index < count; ++index) {
                const KeyType current = DecodeKey(leaf->entries[index]);
                if (!chain_keys.empty() && !comparator_(chain_keys.back(), current)) {
                    return "Leaf chain keys are not strictly ordered";
                }
                chain_keys.push_back(current);
            }
            page_id = bpt_detail::ReadPageId(leaf->next_page_id.data());
        }
        if (chain != leaves) {
            return "Leaf chain is incomplete or differs from in-order traversal";
        }
        return std::nullopt;
        } catch (const DbError& error) {
            return std::string("Page validation failed: ") + error.what();
        }
    }

private:
    static uint16_t ReadCount(const uint8_t* count) noexcept {
        return bpt_detail::ReadU16(count);
    }
    static void WriteCount(uint8_t* count, uint16_t value) noexcept {
        bpt_detail::WriteU16(count, value);
    }
    static const LeafLayout* LeafRead(const ReadPageGuard& guard) {
        if (guard.GetPageType() != PageType::BTREE_LEAF) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Expected B+ tree leaf page",
                          guard.GetPageId());
        }
        return reinterpret_cast<const LeafLayout*>(guard.As<BytePage>()->data());
    }
    static LeafLayout* LeafWrite(WritePageGuard& guard) {
        if (guard.GetPageType() != PageType::BTREE_LEAF) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Expected B+ tree leaf page",
                          guard.GetPageId());
        }
        return reinterpret_cast<LeafLayout*>(guard.AsMut<BytePage>()->data());
    }
    static const InternalLayout* InternalRead(const ReadPageGuard& guard) {
        if (guard.GetPageType() != PageType::BTREE_INTERNAL) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Expected B+ tree internal page",
                          guard.GetPageId());
        }
        return reinterpret_cast<const InternalLayout*>(guard.As<BytePage>()->data());
    }
    static InternalLayout* InternalWrite(WritePageGuard& guard) {
        if (guard.GetPageType() != PageType::BTREE_INTERNAL) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Expected B+ tree internal page",
                          guard.GetPageId());
        }
        return reinterpret_cast<InternalLayout*>(guard.AsMut<BytePage>()->data());
    }
    static void InitializeLeaf(LeafLayout* leaf, page_id_t next_page_id) {
        std::memset(leaf, 0, sizeof(*leaf));
        bpt_detail::WritePageId(leaf->next_page_id.data(), next_page_id);
    }
    static void InitializeInternal(InternalLayout* internal) {
        std::memset(internal, 0, sizeof(*internal));
    }
    static void WriteLeafEntry(LeafEntry& entry, const KeyType& key, const ValueType& value) {
        const auto encoded_key = KeyCodec::Encode(key);
        const auto encoded_value = ValueCodec::Encode(value);
        entry.key = encoded_key;
        entry.value = encoded_value;
    }
    static KeyType DecodeKey(const LeafEntry& entry) {
        return KeyCodec::Decode(entry.key.data());
    }
    static KeyType DecodeKey(const InternalEntry& entry) {
        return KeyCodec::Decode(entry.key.data());
    }
    static void WriteInternalKey(InternalEntry& entry, const KeyType& key) {
        entry.key = KeyCodec::Encode(key);
    }
    static void WriteChild(InternalEntry& entry, page_id_t child) {
        bpt_detail::WritePageId(entry.child_page_id.data(), child);
    }
    bool KeysEqual(const KeyType& lhs, const KeyType& rhs) const {
        return !comparator_(lhs, rhs) && !comparator_(rhs, lhs);
    }
    size_t LeafLowerBound(const LeafLayout* leaf, uint16_t count,
                          const KeyType& key) const {
        const auto found = std::lower_bound(
            leaf->entries.begin(), leaf->entries.begin() + count, key,
            [&](const LeafEntry& entry, const KeyType& wanted) {
                return comparator_(KeyCodec::Decode(entry.key.data()), wanted);
            });
        return static_cast<size_t>(found - leaf->entries.begin());
    }
    size_t InternalChildIndex(const InternalLayout* internal, uint16_t count,
                              const KeyType& key) const {
        const auto found = std::upper_bound(
            internal->entries.begin() + 1,
            internal->entries.begin() + static_cast<std::ptrdiff_t>(count + 1U), key,
            [&](const KeyType& wanted, const InternalEntry& entry) {
                return comparator_(wanted, KeyCodec::Decode(entry.key.data()));
            });
        return static_cast<size_t>(found - (internal->entries.begin() + 1));
    }
    static ReadPageGuard FetchRead(const page_id_t page_id,
                                   const std::shared_ptr<BufferPoolManager>& buffer_pool) {
        ReadPageGuard guard = buffer_pool->FetchRead(page_id);
        if (!guard) {
            throw DbError(ErrorCode::PAGE_OUT_OF_RANGE, "Failed to fetch B+ tree page", page_id);
        }
        return guard;
    }
    ReadPageGuard FetchRead(page_id_t page_id) const {
        return FetchRead(page_id, buffer_pool_);
    }
    WritePageGuard FetchWrite(page_id_t page_id) const {
        WritePageGuard guard = buffer_pool_->FetchWrite(page_id);
        if (!guard) {
            throw DbError(ErrorCode::PAGE_OUT_OF_RANGE, "Failed to fetch B+ tree page", page_id);
        }
        return guard;
    }
    WritePageGuard NewPage() const {
        WritePageGuard guard = buffer_pool_->NewPageGuarded();
        if (!guard) {
            throw DbError(ErrorCode::BUFFER_POOL_EXHAUSTED, "Failed to allocate B+ tree page");
        }
        return guard;
    }

    void InitializeCatalogMeta() {
        if (!buffer_pool_) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "B+ tree requires a buffer pool");
        }
        page_id_t catalog_meta_id = INVALID_PAGE_ID;
        {
            ReadPageGuard header_guard = FetchRead(0);
            const BytePage* header = header_guard.As<BytePage>();
            catalog_meta_id =
                bpt_detail::ReadPageId(reinterpret_cast<const uint8_t*>(header->data() + 28));
        }
        if (catalog_meta_id == INVALID_PAGE_ID) {
            WritePageGuard meta_guard = NewPage();
            const page_id_t new_meta_page_id = meta_guard.GetPageId();
            meta_page_id_ = new_meta_page_id;
            meta_guard.SetPageType(PageType::INDEX_META);
            auto* meta = reinterpret_cast<bpt_detail::IndexMetaLayout*>(
                meta_guard.AsMut<BytePage>()->data());
            bpt_detail::WritePageId(
                reinterpret_cast<uint8_t*>(meta) + sizeof(bpt_detail::IndexMetaLayout),
                INVALID_PAGE_ID);
            bpt_detail::WritePageId(meta->root_page_id.data(), INVALID_PAGE_ID);
            bpt_detail::WriteU16(meta->height.data(), 0);
            bpt_detail::WriteU16(meta->key_kind.data(), static_cast<uint16_t>(key_kind_));
            meta_guard.Drop();

            WritePageGuard header_guard = FetchWrite(0);
            BytePage* header = header_guard.AsMut<BytePage>();
            const page_id_t existing = bpt_detail::ReadPageId(
                reinterpret_cast<const uint8_t*>(header->data() + 28));
            if (existing == INVALID_PAGE_ID) {
                bpt_detail::WritePageId(reinterpret_cast<uint8_t*>(header->data() + 28),
                                        meta_page_id_);
                catalog_root_meta_ = true;
            } else {
                meta_page_id_ = existing;
                catalog_root_meta_ = true;
                header_guard.Drop();
                meta_guard.Drop();
                if (!buffer_pool_->DeletePage(new_meta_page_id)) {
                    throw DbError(ErrorCode::PAGE_CORRUPT,
                                  "Could not retire unused INDEX_META page", new_meta_page_id);
                }
            }
        } else {
            meta_page_id_ = catalog_meta_id;
            catalog_root_meta_ = true;
        }
        LoadMeta();
    }

    void LoadMeta() {
        ReadPageGuard guard = FetchRead(meta_page_id_);
        if (guard.GetPageType() != PageType::INDEX_META) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Expected INDEX_META page", meta_page_id_);
        }
        const auto* meta = reinterpret_cast<const bpt_detail::IndexMetaLayout*>(
            guard.As<BytePage>()->data());
        const uint16_t stored_kind = bpt_detail::ReadU16(meta->key_kind.data());
        if (stored_kind != static_cast<uint16_t>(key_kind_)) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "INDEX_META key kind does not match index");
        }
        root_page_id_ = bpt_detail::ReadPageId(meta->root_page_id.data());
        height_ = bpt_detail::ReadU16(meta->height.data());
        if ((root_page_id_ == INVALID_PAGE_ID) != (height_ == 0)) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "INDEX_META root and height are inconsistent",
                          meta_page_id_);
        }
        if (root_page_id_ != INVALID_PAGE_ID) {
            ascending_inserts_ = false;
        }
    }

    void SetRoot(page_id_t root_page_id, uint16_t height) {
        WritePageGuard guard = FetchWrite(meta_page_id_);
        auto* meta = reinterpret_cast<bpt_detail::IndexMetaLayout*>(
            guard.AsMut<BytePage>()->data());
        bpt_detail::WritePageId(meta->root_page_id.data(), root_page_id);
        bpt_detail::WriteU16(meta->height.data(), height);
        bpt_detail::WriteU16(meta->key_kind.data(), static_cast<uint16_t>(key_kind_));
        root_page_id_ = root_page_id;
        height_ = height;
    }

    page_id_t FindLeaf(const KeyType& key, DescentPath* path) const {
        page_id_t page_id = root_page_id_;
        if (path != nullptr) {
            path->pages.clear();
            path->child_indexes.clear();
        }
        for (uint16_t level = height_; level > 1; --level) {
            if (path != nullptr) {
                path->pages.push_back(page_id);
            }
            ReadPageGuard guard = FetchRead(page_id);
            const auto* internal = InternalRead(guard);
            const uint16_t count = ReadCount(internal->count.data());
            const size_t child_index = InternalChildIndex(internal, count, key);
            page_id = bpt_detail::ReadPageId(internal->entries[child_index].child_page_id.data());
            if (path != nullptr) {
                path->child_indexes.push_back(child_index);
            }
        }
        if (path != nullptr) {
            path->pages.push_back(page_id);
        }
        return page_id;
    }

    void InsertIntoParent(const DescentPath& path, const KeyType& separator,
                          page_id_t right_child) {
        KeyType promoted = separator;
        page_id_t new_right = right_child;
        for (size_t reverse = path.pages.size() - 1U; reverse > 0; --reverse) {
            const size_t parent_level = reverse - 1U;
            const page_id_t parent_id = path.pages[parent_level];
            WritePageGuard parent_guard = FetchWrite(parent_id);
            auto* parent = InternalWrite(parent_guard);
            const uint16_t old_count = ReadCount(parent->count.data());
            const size_t key_index = static_cast<size_t>(std::lower_bound(
                parent->entries.begin() + 1,
                parent->entries.begin() + static_cast<std::ptrdiff_t>(old_count + 1U),
                promoted, [&](const InternalEntry& entry, const KeyType& wanted) {
                    return comparator_(DecodeKey(entry), wanted);
                }) - (parent->entries.begin() + 1));

            if (old_count < kInternalKeyCapacity) {
                std::move_backward(
                    parent->entries.begin() + static_cast<std::ptrdiff_t>(key_index + 1U),
                    parent->entries.begin() + old_count + 1U,
                    parent->entries.begin() + old_count + 2U);
                WriteInternalKey(parent->entries[key_index + 1U], promoted);
                WriteChild(parent->entries[key_index + 1U], new_right);
                WriteCount(parent->count.data(), static_cast<uint16_t>(old_count + 1U));
                return;
            }

            std::vector<KeyType> keys;
            std::vector<page_id_t> children;
            keys.reserve(old_count + 1U);
            children.reserve(old_count + 2U);
            children.push_back(
                bpt_detail::ReadPageId(parent->entries[0].child_page_id.data()));
            for (uint16_t index = 0; index < old_count; ++index) {
                keys.push_back(DecodeKey(parent->entries[index + 1U]));
                children.push_back(bpt_detail::ReadPageId(
                    parent->entries[index + 1U].child_page_id.data()));
            }
            keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(key_index), promoted);
            children.insert(children.begin() + static_cast<std::ptrdiff_t>(key_index + 1U),
                            new_right);

            const size_t middle = keys.size() / 2U;
            promoted = keys[middle];
            WritePageGuard right_guard = NewPage();
            new_right = right_guard.GetPageId();
            right_guard.SetPageType(PageType::BTREE_INTERNAL);
            auto* right = InternalWrite(right_guard);
            InitializeInternal(right);
            const size_t right_key_count = keys.size() - middle - 1U;
            WriteCount(parent->count.data(), static_cast<uint16_t>(middle));
            WriteChild(parent->entries[0], children[0]);
            for (size_t index = 0; index < middle; ++index) {
                WriteInternalKey(parent->entries[index + 1U], keys[index]);
                WriteChild(parent->entries[index + 1U], children[index + 1U]);
            }
            WriteCount(right->count.data(), static_cast<uint16_t>(right_key_count));
            WriteChild(right->entries[0], children[middle + 1U]);
            for (size_t index = 0; index < right_key_count; ++index) {
                WriteInternalKey(right->entries[index + 1U], keys[middle + 1U + index]);
                WriteChild(right->entries[index + 1U],
                           children[middle + 2U + index]);
            }
            right_guard.Drop();
            parent_guard.Drop();

            if (parent_level == 0) {
                WritePageGuard root_guard = NewPage();
                const page_id_t root_id = root_guard.GetPageId();
                root_guard.SetPageType(PageType::BTREE_INTERNAL);
                auto* root = InternalWrite(root_guard);
                InitializeInternal(root);
                WriteCount(root->count.data(), 1);
                WriteChild(root->entries[0], parent_id);
                WriteInternalKey(root->entries[1], promoted);
                WriteChild(root->entries[1], new_right);
                root_guard.Drop();
                SetRoot(root_id, static_cast<uint16_t>(height_ + 1U));
                return;
            }
        }

        WritePageGuard root_guard = NewPage();
        const page_id_t root_id = root_guard.GetPageId();
        root_guard.SetPageType(PageType::BTREE_INTERNAL);
        auto* root = InternalWrite(root_guard);
        InitializeInternal(root);
        WriteCount(root->count.data(), 1);
        WriteChild(root->entries[0], root_page_id_);
        WriteInternalKey(root->entries[1], promoted);
        WriteChild(root->entries[1], new_right);
        root_guard.Drop();
        SetRoot(root_id, static_cast<uint16_t>(height_ + 1U));
    }

    static constexpr uint16_t LeafMinimum() {
        return static_cast<uint16_t>((kLeafCapacity + 9U) / 10U);
    }

    static constexpr uint16_t InternalMinimumKeys() {
        return static_cast<uint16_t>((kInternalChildCapacity + 1U) / 2U - 1U);
    }

    void FixSeparator(const DescentPath& path, const KeyType& new_minimum) {
        for (size_t parent_level = path.child_indexes.size(); parent_level > 0;
             --parent_level) {
            const size_t level = parent_level - 1U;
            const size_t child_index = path.child_indexes[level];
            if (child_index == 0) {
                continue;
            }
            WritePageGuard guard = FetchWrite(path.pages[level]);
            auto* parent = InternalWrite(guard);
            WriteInternalKey(parent->entries[child_index], new_minimum);
            return;
        }
    }

    std::vector<KeyType> InternalKeys(const InternalLayout* internal,
                                      uint16_t count) const {
        std::vector<KeyType> keys;
        keys.reserve(count);
        for (uint16_t index = 0; index < count; ++index) {
            keys.push_back(DecodeKey(internal->entries[index + 1U]));
        }
        return keys;
    }
    static std::vector<page_id_t> InternalChildren(const InternalLayout* internal,
                                                  uint16_t count) {
        std::vector<page_id_t> children;
        children.reserve(static_cast<size_t>(count) + 1U);
        for (uint16_t index = 0; index <= count; ++index) {
            children.push_back(
                bpt_detail::ReadPageId(internal->entries[index].child_page_id.data()));
        }
        return children;
    }
    static void StoreInternal(InternalLayout* internal, const std::vector<KeyType>& keys,
                              const std::vector<page_id_t>& children) {
        InitializeInternal(internal);
        WriteCount(internal->count.data(), static_cast<uint16_t>(keys.size()));
        for (size_t index = 0; index < children.size(); ++index) {
            WriteChild(internal->entries[index], children[index]);
            if (index != 0) {
                WriteInternalKey(internal->entries[index], keys[index - 1U]);
            }
        }
    }

    void RebalanceLeaf(const DescentPath& path) {
        const size_t parent_level = path.pages.size() - 2U;
        const page_id_t parent_id = path.pages[parent_level];
        const size_t child_index = path.child_indexes[parent_level];
        const page_id_t leaf_id = path.pages.back();
        bool merged = false;
        page_id_t retired = INVALID_PAGE_ID;
        std::optional<KeyType> merged_minimum;
        {
            WritePageGuard parent_guard = FetchWrite(parent_id);
            auto* parent = InternalWrite(parent_guard);
            const uint16_t parent_count = ReadCount(parent->count.data());
            WritePageGuard leaf_guard = FetchWrite(leaf_id);
            auto* leaf = LeafWrite(leaf_guard);
            uint16_t leaf_count = ReadCount(leaf->count.data());

            if (child_index > 0) {
                const page_id_t left_id = bpt_detail::ReadPageId(
                    parent->entries[child_index - 1U].child_page_id.data());
                WritePageGuard left_guard = FetchWrite(left_id);
                auto* left = LeafWrite(left_guard);
                const uint16_t left_count = ReadCount(left->count.data());
                if (left_count > LeafMinimum()) {
                    std::move_backward(leaf->entries.begin(),
                                       leaf->entries.begin() + leaf_count,
                                       leaf->entries.begin() + leaf_count + 1U);
                    leaf->entries[0] = left->entries[left_count - 1U];
                    WriteCount(leaf->count.data(), static_cast<uint16_t>(leaf_count + 1U));
                    WriteCount(left->count.data(), static_cast<uint16_t>(left_count - 1U));
                    WriteInternalKey(parent->entries[child_index],
                                     DecodeKey(leaf->entries[0]));
                    return;
                }
            }
            if (child_index < parent_count) {
                const page_id_t right_id = bpt_detail::ReadPageId(
                    parent->entries[child_index + 1U].child_page_id.data());
                WritePageGuard right_guard = FetchWrite(right_id);
                auto* right = LeafWrite(right_guard);
                const uint16_t right_count = ReadCount(right->count.data());
                if (right_count > LeafMinimum()) {
                    leaf->entries[leaf_count] = right->entries[0];
                    ++leaf_count;
                    std::move(right->entries.begin() + 1,
                              right->entries.begin() + right_count, right->entries.begin());
                    WriteCount(leaf->count.data(), leaf_count);
                    WriteCount(right->count.data(), static_cast<uint16_t>(right_count - 1U));
                    WriteInternalKey(parent->entries[child_index + 1U],
                                     DecodeKey(right->entries[0]));
                    return;
                }
            }

            if (child_index > 0) {
                const page_id_t left_id = bpt_detail::ReadPageId(
                    parent->entries[child_index - 1U].child_page_id.data());
                WritePageGuard left_guard = FetchWrite(left_id);
                auto* left = LeafWrite(left_guard);
                const uint16_t left_count = ReadCount(left->count.data());
                std::copy_n(leaf->entries.begin(), leaf_count,
                            left->entries.begin() + left_count);
                WriteCount(left->count.data(),
                           static_cast<uint16_t>(left_count + leaf_count));
                bpt_detail::WritePageId(left->next_page_id.data(),
                                        bpt_detail::ReadPageId(leaf->next_page_id.data()));
                RemoveInternalChild(parent, parent_count, child_index);
                retired = leaf_id;
                merged = true;
            } else {
                const page_id_t right_id = bpt_detail::ReadPageId(
                    parent->entries[child_index + 1U].child_page_id.data());
                WritePageGuard right_guard = FetchWrite(right_id);
                auto* right = LeafWrite(right_guard);
                const uint16_t right_count = ReadCount(right->count.data());
                if (right_count != 0) {
                    merged_minimum = DecodeKey(right->entries[0]);
                }
                std::copy_n(right->entries.begin(), right_count,
                            leaf->entries.begin() + leaf_count);
                WriteCount(leaf->count.data(),
                           static_cast<uint16_t>(leaf_count + right_count));
                bpt_detail::WritePageId(leaf->next_page_id.data(),
                                        bpt_detail::ReadPageId(right->next_page_id.data()));
                RemoveInternalChild(parent, parent_count, child_index + 1U);
                retired = right_id;
                merged = true;
            }
        }
        if (retired != INVALID_PAGE_ID) {
            if (!buffer_pool_->DeletePage(retired)) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Could not retire merged leaf", retired);
            }
        }
        if (merged_minimum) {
            FixSeparator(path, *merged_minimum);
        }
        if (merged) {
            RebalanceInternal(path, parent_level);
        }
    }

    static void RemoveInternalChild(InternalLayout* parent, uint16_t parent_count,
                                    size_t child_index) {
        const size_t entry_index = child_index;
        std::move(parent->entries.begin() + static_cast<std::ptrdiff_t>(entry_index + 1U),
                  parent->entries.begin() + parent_count + 1U,
                  parent->entries.begin() + static_cast<std::ptrdiff_t>(entry_index));
        WriteCount(parent->count.data(), static_cast<uint16_t>(parent_count - 1U));
    }

    void RebalanceInternal(const DescentPath& path, size_t level) {
        page_id_t current_id = path.pages[level];
        if (current_id == root_page_id_) {
            CollapseRootIfNeeded(current_id);
            return;
        }
        while (current_id != root_page_id_) {
            const size_t parent_level = level - 1U;
            const page_id_t parent_id = path.pages[parent_level];
            const size_t child_index = path.child_indexes[parent_level];
            bool done = false;
            bool merged = false;
            page_id_t retired = INVALID_PAGE_ID;
            {
                WritePageGuard parent_guard = FetchWrite(parent_id);
                auto* parent = InternalWrite(parent_guard);
                const uint16_t parent_count = ReadCount(parent->count.data());
                WritePageGuard current_guard = FetchWrite(current_id);
                auto* current = InternalWrite(current_guard);
                const uint16_t current_count = ReadCount(current->count.data());
                if (current_count >= InternalMinimumKeys()) {
                    return;
                }

                if (child_index > 0) {
                    const page_id_t left_id = bpt_detail::ReadPageId(
                        parent->entries[child_index - 1U].child_page_id.data());
                    WritePageGuard left_guard = FetchWrite(left_id);
                    auto* left = InternalWrite(left_guard);
                    const uint16_t left_count = ReadCount(left->count.data());
                    if (left_count > InternalMinimumKeys()) {
                        std::vector<KeyType> left_keys = InternalKeys(left, left_count);
                        std::vector<page_id_t> left_children = InternalChildren(left, left_count);
                        std::vector<KeyType> current_keys = InternalKeys(current, current_count);
                        std::vector<page_id_t> current_children =
                            InternalChildren(current, current_count);
                        const KeyType parent_separator = DecodeKey(parent->entries[child_index]);
                        current_keys.insert(current_keys.begin(), parent_separator);
                        current_children.insert(current_children.begin(), left_children.back());
                        WriteInternalKey(parent->entries[child_index], left_keys.back());
                        left_keys.pop_back();
                        left_children.pop_back();
                        StoreInternal(left, left_keys, left_children);
                        StoreInternal(current, current_keys, current_children);
                        return;
                    }
                }
                if (child_index < parent_count) {
                    const page_id_t right_id = bpt_detail::ReadPageId(
                        parent->entries[child_index + 1U].child_page_id.data());
                    WritePageGuard right_guard = FetchWrite(right_id);
                    auto* right = InternalWrite(right_guard);
                    const uint16_t right_count = ReadCount(right->count.data());
                    if (right_count > InternalMinimumKeys()) {
                        std::vector<KeyType> current_keys = InternalKeys(current, current_count);
                        std::vector<page_id_t> current_children =
                            InternalChildren(current, current_count);
                        std::vector<KeyType> right_keys = InternalKeys(right, right_count);
                        std::vector<page_id_t> right_children = InternalChildren(right, right_count);
                        current_keys.push_back(DecodeKey(parent->entries[child_index + 1U]));
                        current_children.push_back(right_children.front());
                        WriteInternalKey(parent->entries[child_index + 1U], right_keys.front());
                        right_keys.erase(right_keys.begin());
                        right_children.erase(right_children.begin());
                        StoreInternal(current, current_keys, current_children);
                        StoreInternal(right, right_keys, right_children);
                        return;
                    }
                }

                if (child_index > 0) {
                    const page_id_t left_id = bpt_detail::ReadPageId(
                        parent->entries[child_index - 1U].child_page_id.data());
                    WritePageGuard left_guard = FetchWrite(left_id);
                    auto* left = InternalWrite(left_guard);
                    const uint16_t left_count = ReadCount(left->count.data());
                    std::vector<KeyType> keys = InternalKeys(left, left_count);
                    std::vector<page_id_t> children = InternalChildren(left, left_count);
                    std::vector<KeyType> current_keys = InternalKeys(current, current_count);
                    std::vector<page_id_t> current_children =
                        InternalChildren(current, current_count);
                    keys.push_back(DecodeKey(parent->entries[child_index]));
                    keys.insert(keys.end(), current_keys.begin(), current_keys.end());
                    children.insert(children.end(), current_children.begin(),
                                    current_children.end());
                    StoreInternal(left, keys, children);
                    RemoveInternalChild(parent, parent_count, child_index);
                    retired = current_id;
                    merged = true;
                } else {
                    const page_id_t right_id = bpt_detail::ReadPageId(
                        parent->entries[child_index + 1U].child_page_id.data());
                    WritePageGuard right_guard = FetchWrite(right_id);
                    auto* right = InternalWrite(right_guard);
                    const uint16_t right_count = ReadCount(right->count.data());
                    std::vector<KeyType> keys = InternalKeys(current, current_count);
                    std::vector<page_id_t> children = InternalChildren(current, current_count);
                    std::vector<KeyType> right_keys = InternalKeys(right, right_count);
                    std::vector<page_id_t> right_children = InternalChildren(right, right_count);
                    keys.push_back(DecodeKey(parent->entries[child_index + 1U]));
                    keys.insert(keys.end(), right_keys.begin(), right_keys.end());
                    children.insert(children.end(), right_children.begin(), right_children.end());
                    StoreInternal(current, keys, children);
                    RemoveInternalChild(parent, parent_count, child_index + 1U);
                    retired = right_id;
                    merged = true;
                }
            }
            if (retired != INVALID_PAGE_ID) {
                if (!buffer_pool_->DeletePage(retired)) {
                    throw DbError(ErrorCode::PAGE_CORRUPT,
                                  "Could not retire merged internal node", retired);
                }
            }
            if (!merged) {
                done = true;
            }
            if (done) {
                return;
            }
            current_id = parent_id;
            level = parent_level;
            if (current_id == root_page_id_) {
                CollapseRootIfNeeded(current_id);
                return;
            }
        }
    }

    void CollapseRootIfNeeded(page_id_t root_id) {
        WritePageGuard root_guard = FetchWrite(root_id);
        auto* root = InternalWrite(root_guard);
        if (ReadCount(root->count.data()) != 0) {
            return;
        }
        const page_id_t only_child =
            bpt_detail::ReadPageId(root->entries[0].child_page_id.data());
        root_guard.Drop();
        if (!buffer_pool_->DeletePage(root_id)) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Could not retire collapsed root", root_id);
        }
        SetRoot(only_child, static_cast<uint16_t>(height_ - 1U));
    }

    std::shared_ptr<BufferPoolManager> buffer_pool_;
    KeyComparator comparator_;
    IndexKeyKind key_kind_;
    page_id_t meta_page_id_{INVALID_PAGE_ID};
    page_id_t root_page_id_{INVALID_PAGE_ID};
    uint16_t height_{0};
    bool ascending_inserts_{true};
    bool catalog_root_meta_{false};
    std::optional<KeyType> last_inserted_key_;
};

}  // namespace relational_db

#endif
