#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "db_error.h"
#include "disk_manager.h"
#include "slotted_page.h"
#include "storage/table_heap.h"
#include "storage/tuple.h"

namespace {

using relational_db::BufferPoolManager;
using relational_db::Column;
using relational_db::ColumnType;
using relational_db::DbError;
using relational_db::ErrorCode;
using relational_db::FileDiskManager;
using relational_db::MAX_TUPLE_SIZE;
using relational_db::PAGE_DATA_SIZE;
using relational_db::RID;
using relational_db::Row;
using relational_db::Schema;
using relational_db::SlottedPage;
using relational_db::TableHeap;
using relational_db::Tuple;

class TemporaryDatabase {
   public:
    TemporaryDatabase() {
        static std::atomic<uint64_t> next_id{0};
        path_ = std::filesystem::temp_directory_path() /
                ("custom-db-storage-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(next_id.fetch_add(1)));
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    std::string String() const {
        return path_.string();
    }

   private:
    std::filesystem::path path_;
};

Tuple RandomTuple(std::mt19937_64& random, size_t size) {
    Tuple tuple(size);
    for (std::byte& byte : tuple) {
        byte = static_cast<std::byte>(random() & 0xffU);
    }
    return tuple;
}

void ExpectFullScan(TableHeap& heap, const std::unordered_map<RID, Tuple>& expected) {
    std::unordered_map<RID, Tuple> actual;
    auto iterator = heap.Begin();
    RID rid;
    Tuple tuple;
    while (iterator.Next(rid, tuple)) {
        ASSERT_TRUE(actual.emplace(rid, tuple).second);
    }
    ASSERT_EQ(actual.size(), expected.size());
    for (const auto& entry : expected) {
        const auto found = actual.find(entry.first);
        ASSERT_NE(found, actual.end());
        EXPECT_EQ(found->second, entry.second);
    }
}

Row RandomRow(const Schema& schema, std::mt19937_64& random) {
    Row row;
    row.reserve(schema.columns.size());
    std::uniform_int_distribution<int> value_kind(0, 4);
    std::uniform_int_distribution<int32_t> int_value(INT32_MIN, INT32_MAX);
    std::uniform_int_distribution<int64_t> long_value(INT64_MIN, INT64_MAX);
    std::uniform_real_distribution<double> double_value(-1.0e12, 1.0e12);
    for (size_t index = 0; index < schema.columns.size(); ++index) {
        const Column& column = schema.columns[index];
        const bool allow_null = !schema.primary_key || *schema.primary_key != index;
        if (allow_null && value_kind(random) == 0) {
            row.emplace_back(std::monostate{});
            continue;
        }
        switch (column.type) {
            case ColumnType::INT:
                row.emplace_back(int_value(random));
                break;
            case ColumnType::BIGINT:
                row.emplace_back(long_value(random));
                break;
            case ColumnType::DOUBLE:
                row.emplace_back(double_value(random));
                break;
            case ColumnType::BOOLEAN:
                row.emplace_back((random() & 1U) != 0);
                break;
            case ColumnType::VARCHAR: {
                std::uniform_int_distribution<size_t> length(0, column.varchar_length);
                const size_t size = length(random);
                std::string text(size, '\0');
                for (char& character : text) {
                    character = static_cast<char>(random() & 0xffU);
                }
                row.emplace_back(std::move(text));
                break;
            }
        }
    }
    return row;
}

TEST(StorageTupleTest, RandomSchemasRoundTripWithNullsAndEmptyStrings) {
    constexpr uint64_t seed = 0x5eed1234ULL;
    std::cout << "Tuple round-trip seed: " << seed << '\n';
    std::mt19937_64 random(seed);
    std::uniform_int_distribution<size_t> column_count(1, 12);
    std::uniform_int_distribution<int> column_type(0, 4);
    std::uniform_int_distribution<int> varchar_length(0, 255);
    std::uniform_int_distribution<int> has_primary_key(0, 1);

    for (size_t round = 0; round < 2000; ++round) {
        Schema schema;
        const size_t count = column_count(random);
        for (size_t index = 0; index < count; ++index) {
            const auto type = static_cast<ColumnType>(column_type(random));
            const uint16_t length =
                type == ColumnType::VARCHAR ? static_cast<uint16_t>(varchar_length(random)) : 0;
            schema.columns.push_back(Column{"c" + std::to_string(index), type, length});
        }
        if (has_primary_key(random) != 0) {
            std::uniform_int_distribution<size_t> key_column(0, count - 1);
            schema.primary_key = key_column(random);
        }
        const Row row = RandomRow(schema, random);
        const Tuple encoded = relational_db::SerializeTuple(schema, row);
        const Row decoded = relational_db::DeserializeTuple(schema, encoded);
        EXPECT_EQ(relational_db::SerializeTuple(schema, decoded), encoded);
    }
}

TEST(StorageTupleTest, MaximumTupleAndOneByteOverAreChecked) {
    Schema schema;
    Row row;
    for (size_t index = 0; index < 16; ++index) {
        schema.columns.push_back(Column{"v" + std::to_string(index), ColumnType::VARCHAR, 255});
        row.emplace_back(std::string(index == 15 ? 176U : 255U, 'x'));
    }
    const Tuple maximum = relational_db::SerializeTuple(schema, row);
    ASSERT_EQ(maximum.size(), MAX_TUPLE_SIZE);
    EXPECT_EQ(
        relational_db::SerializeTuple(schema, relational_db::DeserializeTuple(schema, maximum)),
        maximum);

    std::get<std::string>(row.back()).push_back('x');
    try {
        static_cast<void>(relational_db::SerializeTuple(schema, row));
        FAIL() << "Expected an oversized tuple to be rejected";
    } catch (const DbError& error) {
        EXPECT_EQ(error.Code(), ErrorCode::TUPLE_TOO_LARGE);
    }
}

TEST(StorageTupleTest, RejectsNullPrimaryKeyAndInvalidVarcharLength) {
    Schema schema{{Column{"id", ColumnType::INT}}, 0};
    EXPECT_THROW(relational_db::SerializeTuple(schema, Row{std::monostate{}}), DbError);

    Schema invalid{{Column{"name", ColumnType::VARCHAR, 256}}, std::nullopt};
    EXPECT_THROW(invalid.Validate(), DbError);
    EXPECT_THROW(relational_db::SerializeTuple(invalid, Row{std::string(256, 'x')}), DbError);
    EXPECT_THROW(relational_db::SerializeTuple(
                     Schema{{Column{"id", ColumnType::INT}}, std::nullopt}, Row{int64_t{1}}),
                 DbError);
    EXPECT_THROW(
        (Schema{{Column{"id", ColumnType::INT}, Column{"id", ColumnType::BIGINT}}, std::nullopt})
            .Validate(),
        DbError);
}

TEST(StorageTupleTest, EncodesIntegersAndDoubleAsLittleEndian) {
    Schema schema{{Column{"i", ColumnType::INT}, Column{"b", ColumnType::BIGINT},
                   Column{"d", ColumnType::DOUBLE}, Column{"ok", ColumnType::BOOLEAN}},
                  std::nullopt};
    const Row row{int32_t{0x01020304}, int64_t{0x0102030405060708LL}, 1.0, true};
    const Tuple tuple = relational_db::SerializeTuple(schema, row);
    const std::array<uint8_t, 4> expected_int{0x04, 0x03, 0x02, 0x01};
    const std::array<uint8_t, 8> expected_bigint{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    const std::array<uint8_t, 8> expected_double{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f};
    for (size_t byte = 0; byte < expected_int.size(); ++byte) {
        EXPECT_EQ(std::to_integer<uint8_t>(tuple[1 + byte]), expected_int[byte]);
    }
    for (size_t byte = 0; byte < expected_bigint.size(); ++byte) {
        EXPECT_EQ(std::to_integer<uint8_t>(tuple[5 + byte]), expected_bigint[byte]);
        EXPECT_EQ(std::to_integer<uint8_t>(tuple[13 + byte]), expected_double[byte]);
    }
    EXPECT_EQ(std::to_integer<uint8_t>(tuple.back()), 1);
}

TEST(SlottedPageTest, FullPageOneByteAndZeroLengthTuples) {
    std::array<char, PAGE_DATA_SIZE> body{};
    SlottedPage page(body.data());
    page.Init();
    std::vector<std::byte> one_byte{std::byte{0x42}};
    const uint16_t one_byte_slot = page.InsertTuple(one_byte.data(), one_byte.size());
    ASSERT_NE(one_byte_slot, SlottedPage::INVALID_SLOT_ID);
    std::vector<std::byte> result;
    ASSERT_TRUE(page.ReadTuple(one_byte_slot, result));
    EXPECT_EQ(result, one_byte);

    std::array<char, PAGE_DATA_SIZE> full_body{};
    SlottedPage full_page(full_body.data());
    full_page.Init();
    Tuple maximum(MAX_TUPLE_SIZE, std::byte{0x7f});
    const uint16_t full_slot = full_page.InsertTuple(maximum.data(), maximum.size());
    ASSERT_NE(full_slot, SlottedPage::INVALID_SLOT_ID);
    EXPECT_EQ(full_page.GetFreeSpaceRemaining(), 0);
    ASSERT_TRUE(full_page.ReadTuple(full_slot, result));
    EXPECT_EQ(result, maximum);

    std::array<char, PAGE_DATA_SIZE> empty_body{};
    SlottedPage empty_page(empty_body.data());
    empty_page.Init();
    const uint16_t empty_slot = empty_page.InsertTuple(nullptr, 0);
    ASSERT_NE(empty_slot, SlottedPage::INVALID_SLOT_ID);
    ASSERT_TRUE(empty_page.ReadTuple(empty_slot, result));
    EXPECT_TRUE(result.empty());
    EXPECT_TRUE(empty_page.DeleteTuple(empty_slot));
    EXPECT_FALSE(empty_page.ReadTuple(empty_slot, result));
    EXPECT_EQ(empty_page.InsertTuple(nullptr, 0), empty_slot);
    ASSERT_TRUE(empty_page.ReadTuple(empty_slot, result));
    EXPECT_TRUE(result.empty());
}

TEST(SlottedPageTest, CompactionPreservesSlotsAndUpdatesInPlaceWhenPossible) {
    std::array<char, PAGE_DATA_SIZE> body{};
    SlottedPage page(body.data());
    page.Init();
    Tuple first(100, std::byte{0x11});
    Tuple middle(200, std::byte{0x22});
    Tuple last(300, std::byte{0x33});
    const uint16_t first_slot = page.InsertTuple(first.data(), first.size());
    const uint16_t middle_slot = page.InsertTuple(middle.data(), middle.size());
    const uint16_t last_slot = page.InsertTuple(last.data(), last.size());
    ASSERT_TRUE(page.DeleteTuple(first_slot));
    ASSERT_TRUE(page.DeleteTuple(middle_slot));
    page.Compact();

    std::vector<std::byte> result;
    ASSERT_TRUE(page.ReadTuple(last_slot, result));
    EXPECT_EQ(result, last);
    Tuple replacement(500, std::byte{0x44});
    EXPECT_EQ(page.UpdateTuple(last_slot, replacement.data(), replacement.size()),
              SlottedPage::UpdateResult::UPDATED);
    ASSERT_TRUE(page.ReadTuple(last_slot, result));
    EXPECT_EQ(result, replacement);
    Tuple too_large(MAX_TUPLE_SIZE, std::byte{0x55});
    EXPECT_EQ(page.UpdateTuple(last_slot, too_large.data(), too_large.size()),
              SlottedPage::UpdateResult::NEEDS_MOVE);
}

TEST(TableHeapTest, PersistsRowsMovesUpdatesAndScans) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    BufferPoolManager buffer_pool(4, disk);
    TableHeap heap = TableHeap::Create(buffer_pool);
    Tuple small(16, std::byte{0x12});
    const RID first = heap.Insert(small);
    const RID second = heap.Insert(Tuple{});
    EXPECT_EQ(heap.Get(first), std::optional<Tuple>(small));
    EXPECT_EQ(heap.Get(second), std::optional<Tuple>(Tuple{}));

    Tuple in_place(24, std::byte{0x34});
    ASSERT_EQ(heap.Update(first, in_place), std::optional<RID>(first));
    EXPECT_EQ(heap.Get(first), std::optional<Tuple>(in_place));

    Tuple large(MAX_TUPLE_SIZE, std::byte{0x56});
    const auto moved = heap.Update(first, large);
    ASSERT_TRUE(moved);
    EXPECT_NE(*moved, first);
    EXPECT_FALSE(heap.Get(first));
    EXPECT_EQ(heap.Get(*moved), std::optional<Tuple>(large));

    std::unordered_map<RID, Tuple> expected{{second, Tuple{}}, {*moved, large}};
    ExpectFullScan(heap, expected);
    EXPECT_TRUE(heap.Delete(second));
    EXPECT_FALSE(heap.Get(second));
    EXPECT_FALSE(heap.Delete(second));
    expected.erase(second);
    ExpectFullScan(heap, expected);
}

TEST(TableHeapTest, RebuildsReclaimedPagesAfterReopen) {
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    relational_db::page_id_t header_page = relational_db::INVALID_PAGE_ID;
    RID first;
    RID last;
    {
        BufferPoolManager buffer_pool(4, disk);
        TableHeap heap = TableHeap::Create(buffer_pool);
        header_page = heap.HeaderPageId();
        Tuple full(MAX_TUPLE_SIZE, std::byte{0x61});
        first = heap.Insert(full);
        last = heap.Insert(full);
        ASSERT_NE(first.page_id, last.page_id);
        ASSERT_TRUE(heap.Delete(first));
        buffer_pool.FlushAll();
    }
    {
        BufferPoolManager buffer_pool(4, disk);
        TableHeap heap(buffer_pool, header_page);
        const RID reused = heap.Insert(Tuple{std::byte{0x01}});
        EXPECT_EQ(reused.page_id, first.page_id);
    }
}

TEST(RIDTest, PacksAndUnpacksPageAndSlot) {
    const RID rid{0x123456, 0xabcd};
    EXPECT_EQ(RID::Unpack(rid.Pack()), rid);
    EXPECT_THROW((RID{relational_db::INVALID_PAGE_ID, 1}.Pack()), DbError);
    EXPECT_THROW(RID::Unpack(uint64_t{1} << 48U), DbError);
}

TEST(StorageFuzzTest, MillionOperationsAgainstRidOracle) {
    constexpr uint64_t seed = 0x51a77eULL;
    constexpr size_t operation_count = 1'000'000;
    std::cout << "Table heap fuzz seed: " << seed << '\n';
    TemporaryDatabase database;
    auto disk = std::make_shared<FileDiskManager>(database.String());
    BufferPoolManager buffer_pool(4, disk);
    TableHeap heap = TableHeap::Create(buffer_pool);
    std::mt19937_64 random(seed);
    std::uniform_int_distribution<size_t> tuple_size(0, MAX_TUPLE_SIZE);
    std::unordered_map<RID, Tuple> oracle;
    std::vector<RID> live;
    live.reserve(2048);

    for (size_t operation = 0; operation < operation_count; ++operation) {
        const unsigned int kind = static_cast<unsigned int>(random() % 4U);
        if (live.empty() || kind == 0U) {
            Tuple tuple = RandomTuple(random, tuple_size(random));
            const RID rid = heap.Insert(tuple);
            ASSERT_EQ(oracle.count(rid), 0U);
            oracle.emplace(rid, tuple);
            live.push_back(rid);
        } else if (kind == 1U) {
            const size_t index = static_cast<size_t>(random() % live.size());
            const RID old_rid = live[index];
            Tuple tuple = RandomTuple(random, tuple_size(random));
            const auto updated = heap.Update(old_rid, tuple);
            ASSERT_TRUE(updated);
            oracle.erase(old_rid);
            oracle[*updated] = tuple;
            live[index] = *updated;
        } else if (kind == 2U) {
            const size_t index = static_cast<size_t>(random() % live.size());
            const RID rid = live[index];
            ASSERT_TRUE(heap.Delete(rid));
            oracle.erase(rid);
            live[index] = live.back();
            live.pop_back();
        } else {
            const RID rid = live[static_cast<size_t>(random() % live.size())];
            const auto actual = heap.Get(rid);
            ASSERT_TRUE(actual);
            EXPECT_EQ(*actual, oracle.at(rid));
        }

        if ((operation + 1U) % 10'000U == 0U) {
            ExpectFullScan(heap, oracle);
        }
    }
}

}  // namespace
