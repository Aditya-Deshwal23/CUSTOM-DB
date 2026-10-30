#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "b_plus_tree.h"
#include "disk_manager.h"

namespace {

using relational_db::BPlusTree;
using relational_db::BufferPoolManager;
using relational_db::DbError;
using relational_db::DiskManager;
using relational_db::ErrorCode;
using relational_db::EncodeIndexKey;
using relational_db::EncodeNonUniqueIndexKey;
using relational_db::IndexKey16;
using relational_db::IndexKey16Less;
using relational_db::IndexKeyKind;

class TemporaryDatabase {
public:
    TemporaryDatabase() {
        static std::atomic<uint64_t> sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("custom-db-btree-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1)));
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + ".wal", error);
    }

    std::string Path() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

using Tree = BPlusTree<int64_t, int64_t, std::less<int64_t>>;

std::shared_ptr<BufferPoolManager> MakePool(const std::string& path);

TEST(IndexKeyTest, SignedValuesAndNonUniqueRidsKeepMemcmpOrder) {
    const IndexKey16 minimum = EncodeIndexKey(std::numeric_limits<int32_t>::min());
    const IndexKey16 negative = EncodeIndexKey(int32_t{-1});
    const IndexKey16 zero = EncodeIndexKey(int32_t{0});
    const IndexKey16 maximum = EncodeIndexKey(std::numeric_limits<int32_t>::max());
    const IndexKey16Less less;
    EXPECT_TRUE(less(minimum, negative));
    EXPECT_TRUE(less(negative, zero));
    EXPECT_TRUE(less(zero, maximum));

    const IndexKey16 first = EncodeNonUniqueIndexKey(7, 10);
    const IndexKey16 second = EncodeNonUniqueIndexKey(7, 11);
    const IndexKey16 larger_value = EncodeNonUniqueIndexKey(8, 1);
    EXPECT_TRUE(less(first, second));
    EXPECT_TRUE(less(second, larger_value));
}

TEST(BPlusTreeTest, SignedInt64KeysAndNegativeRangeOperations) {
    TemporaryDatabase database;
    auto buffer_pool = MakePool(database.Path());
    BPlusTree<int64_t, int64_t, std::less<int64_t>> tree(buffer_pool, std::less<int64_t>());

    for (const int64_t key : { -10LL, -5LL, -1LL, 0LL, 1LL, 2LL, 8LL, 9LL }) {
        ASSERT_TRUE(tree.Insert(key, key * 10LL));
    }

    const auto found = tree.GetValue(-1LL);
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, -10LL);

    const auto range = tree.RangeScan(-5LL, 8LL, true, false);
    ASSERT_EQ(range.size(), 5U);
    EXPECT_EQ(range.front().first, -5LL);
    EXPECT_EQ(range.back().first, 2LL);
    EXPECT_FALSE(tree.Validate());
    buffer_pool->AssertNoPinsHeld();
}

TEST(BPlusTreeTest, IndexKey16UniqueAndRangeOperations) {
    TemporaryDatabase database;
    auto buffer_pool = MakePool(database.Path());
    BPlusTree<IndexKey16, uint64_t, IndexKey16Less> tree(
        buffer_pool, IndexKey16Less(), IndexKeyKind::INT32);

    for (int32_t key = -100; key < 100; ++key) {
        ASSERT_TRUE(tree.Insert(EncodeIndexKey(key), static_cast<uint64_t>(key + 100)));
    }
    const auto found = tree.GetValue(EncodeIndexKey(int32_t{-1}));
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, 99U);
    try {
        (void)tree.Insert(EncodeIndexKey(int32_t{-1}), 777U);
        FAIL() << "duplicate unique key unexpectedly succeeded";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::DUPLICATE_KEY);
    }
    const auto range = tree.RangeScan(EncodeIndexKey(int32_t{-2}),
                                      EncodeIndexKey(int32_t{3}));
    ASSERT_EQ(range.size(), 5U);
    for (size_t index = 0; index < range.size(); ++index) {
        EXPECT_EQ(relational_db::DecodeIndexKeyInt32(range[index].first),
                  static_cast<int32_t>(index) - 2);
    }
    EXPECT_FALSE(tree.Validate());
    buffer_pool->AssertNoPinsHeld();
}

std::shared_ptr<BufferPoolManager> MakePool(const std::string& path) {
    return std::make_shared<BufferPoolManager>(16, std::make_shared<DiskManager>(path));
}

TEST(BPlusTreeFuzzTest, MillionOperationsAgainstMapWithConstantEviction) {
    TemporaryDatabase database;
    auto buffer_pool = MakePool(database.Path());
    Tree tree(buffer_pool, std::less<int64_t>());
    std::map<int64_t, int64_t> oracle;
    constexpr uint32_t seed = 20261030;
    std::cout << "B+ tree fuzz seed: " << seed << std::endl;
    std::mt19937_64 random(seed);
    std::uniform_int_distribution<int64_t> key_distribution(0, 30000);
    std::uniform_int_distribution<int64_t> value_distribution(
        0, std::numeric_limits<int64_t>::max());
    std::uniform_int_distribution<unsigned> operation_distribution(0, 3);

    for (uint32_t operation = 0; operation < 1000000; ++operation) {
        const int64_t key = key_distribution(random);
        switch (operation_distribution(random)) {
            case 0: {
                const int64_t value = value_distribution(random);
                const auto existing = oracle.find(key);
                if (existing == oracle.end()) {
                    ASSERT_TRUE(tree.Insert(key, value)) << "operation=" << operation;
                    oracle.emplace(key, value);
                } else {
                    try {
                        (void)tree.Insert(key, value);
                        FAIL() << "duplicate insert unexpectedly succeeded";
                    } catch (const DbError& error) {
                        EXPECT_EQ(error.Code(), ErrorCode::DUPLICATE_KEY)
                            << "operation=" << operation;
                    }
                }
                break;
            }
            case 1: {
                const bool expected = oracle.erase(key) != 0;
                EXPECT_EQ(tree.Delete(key), expected) << "operation=" << operation;
                break;
            }
            case 2: {
                const auto expected = oracle.find(key);
                const auto actual = tree.GetValue(key);
                EXPECT_EQ(actual.has_value(), expected != oracle.end())
                    << "operation=" << operation;
                if (expected != oracle.end() && actual) {
                    EXPECT_EQ(*actual, expected->second) << "operation=" << operation;
                }
                break;
            }
            default: {
                const int64_t high = key + static_cast<int64_t>(random() % 129U);
                const bool include_low = (random() & 1U) != 0;
                const bool include_high = (random() & 1U) != 0;
                const auto actual = tree.RangeScan(key, high, include_low, include_high);
                std::vector<std::pair<int64_t, int64_t>> expected;
                auto iter = oracle.lower_bound(key);
                if (!include_low && iter != oracle.end() && iter->first == key) {
                    ++iter;
                }
                for (; iter != oracle.end(); ++iter) {
                    if (iter->first > high || (!include_high && iter->first == high)) {
                        break;
                    }
                    expected.push_back(*iter);
                }
                EXPECT_EQ(actual, expected) << "operation=" << operation;
                break;
            }
        }

        buffer_pool->AssertNoPinsHeld();
        if ((operation + 1U) % 1000U == 0) {
            const auto validation_error = tree.Validate();
            ASSERT_FALSE(validation_error) << "operation=" << operation << ": "
                                           << (validation_error ? *validation_error : "");
            buffer_pool->AssertNoPinsHeld();
        }
    }
}

TEST(BPlusTreeTest, AscendingAndRandomFillFactorsBeatHalfFullBaseline) {
    {
        TemporaryDatabase database;
        auto buffer_pool = MakePool(database.Path());
        Tree tree(buffer_pool, std::less<int64_t>());
        for (int64_t key = 0; key < 40000; ++key) {
            ASSERT_TRUE(tree.Insert(key, key));
        }
        ASSERT_FALSE(tree.Validate());
        std::cout << "Ascending leaf fill factor: " << tree.FillFactor() << std::endl;
        EXPECT_GE(tree.FillFactor(), 0.85);
    }
    {
        TemporaryDatabase database;
        auto buffer_pool = MakePool(database.Path());
        Tree tree(buffer_pool, std::less<int64_t>());
        std::vector<int64_t> keys(200000);
        for (size_t index = 0; index < keys.size(); ++index) {
            keys[index] = static_cast<int64_t>(index);
        }
        constexpr uint32_t seed = 20261031;
        std::cout << "B+ tree random-fill seed: " << seed << std::endl;
        std::mt19937 random(seed);
        std::shuffle(keys.begin(), keys.end(), random);
        for (const int64_t key : keys) {
            ASSERT_TRUE(tree.Insert(key, key));
        }
        ASSERT_FALSE(tree.Validate());
        std::cout << "Random leaf fill factor: " << tree.FillFactor() << std::endl;
        EXPECT_GE(tree.FillFactor(), 0.65);
        EXPECT_LE(tree.FillFactor(), 0.72);
    }
}

TEST(BPlusTreeTest, DescendingAndHotKeyChurnRemainValidAfterDeletes) {
    TemporaryDatabase database;
    auto buffer_pool = MakePool(database.Path());
    Tree tree(buffer_pool, std::less<int64_t>());
    for (int64_t key = 20000; key >= 0; --key) {
        ASSERT_TRUE(tree.Insert(key, key));
    }
    ASSERT_FALSE(tree.Validate());
    for (int64_t key = 0; key <= 20000; ++key) {
        ASSERT_TRUE(tree.Delete(key));
        if (key % 500 == 0) {
            const auto error = tree.Validate();
            ASSERT_FALSE(error) << (error ? *error : "");
        }
        buffer_pool->AssertNoPinsHeld();
    }
    EXPECT_TRUE(tree.IsEmpty());
    ASSERT_FALSE(tree.Validate());

    for (int iteration = 0; iteration < 20000; ++iteration) {
        const int64_t key = iteration % 32;
        ASSERT_TRUE(tree.Insert(key, iteration));
        ASSERT_TRUE(tree.Delete(key));
        buffer_pool->AssertNoPinsHeld();
    }
    EXPECT_TRUE(tree.IsEmpty());
    ASSERT_FALSE(tree.Validate());
}

}  // namespace
