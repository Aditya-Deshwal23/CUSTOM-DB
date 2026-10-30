#include "executor.h"

#include <charconv>
#include <iostream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace relational_db {
namespace {

bool ParseInt32(const std::string& token, int32_t& value) {
    const char* const first = token.data();
    const char* const last = first + token.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool ParseInt64(const std::string& token, int64_t& value) {
    const char* const first = token.data();
    const char* const last = first + token.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

}  // namespace

const Schema& Executor::SqlSchema() {
    static const Schema schema{
        {Column{"key", ColumnType::INT}, Column{"value", ColumnType::BIGINT}}, std::nullopt};
    return schema;
}

void Executor::InitializeHeap() {
    page_id_t heap_header_page_id = INVALID_PAGE_ID;
    {
        ReadPageGuard meta_guard = bpm_->FetchRead(tree_.MetaPageId());
        const auto* meta = meta_guard.As<std::array<char, PAGE_DATA_SIZE>>();
        heap_header_page_id = bpt_detail::ReadPageId(
            reinterpret_cast<const uint8_t*>(meta->data() + sizeof(bpt_detail::IndexMetaLayout)));
    }

    if (heap_header_page_id == INVALID_PAGE_ID) {
        TableHeap heap = TableHeap::Create(*bpm_);
        heap_header_page_id = heap.HeaderPageId();
        heap_ = std::make_unique<TableHeap>(std::move(heap));
        WritePageGuard meta_guard = bpm_->FetchWrite(tree_.MetaPageId());
        auto* meta = meta_guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        bpt_detail::WritePageId(
            reinterpret_cast<uint8_t*>(meta->data() + sizeof(bpt_detail::IndexMetaLayout)),
            heap_header_page_id);
    } else {
        heap_ = std::make_unique<TableHeap>(*bpm_, heap_header_page_id);
    }
}

static std::string nextToken(const std::string& sv, size_t& pos) {
    while (pos < sv.size() && sv[pos] == ' ') ++pos;
    if (pos >= sv.size()) return "";
    
    size_t start = pos;
    while (pos < sv.size() && sv[pos] != ' ') ++pos;
    return sv.substr(start, pos - start);
}

std::string Executor::ExecuteQuery(const std::string& query) {
    size_t pos = 0;
    std::string command = nextToken(query, pos);

    if (command == "INSERT") {
        std::string key_s = nextToken(query, pos);
        std::string val_s = nextToken(query, pos);
        int32_t key = 0;
        int64_t value = 0;
        if (!ParseInt32(key_s, key) || !ParseInt64(val_s, value)) {
            return "Syntax error: INSERT <key> <value>";
        }
        std::unique_lock<std::shared_mutex> lock(statement_gate_);
        const IndexKey16 index_key = EncodeIndexKey(key);
        if (tree_.GetValue(index_key)) {
            throw DbError(ErrorCode::DUPLICATE_KEY, "Duplicate key in SQL index");
        }
        const Row row{key, value};
        const Tuple tuple = SerializeTuple(SqlSchema(), row);
        const RID rid = heap_->Insert(tuple);
        tree_.Insert(index_key, rid.Pack());
        return "Inserted (" + std::to_string(key) + ", " + std::to_string(value) + ")";
    } else if (command == "SELECT") {
        std::string key_s = nextToken(query, pos);
        int32_t key = 0;
        if (!ParseInt32(key_s, key)) {
            return "Syntax error: SELECT <key>";
        }
        std::shared_lock<std::shared_mutex> lock(statement_gate_);
        const std::optional<uint64_t> packed_rid = tree_.GetValue(EncodeIndexKey(key));
        if (packed_rid) {
            const std::optional<Tuple> tuple = heap_->Get(RID::Unpack(*packed_rid));
            if (!tuple) {
                throw DbError(ErrorCode::PAGE_CORRUPT,
                              "Index RID does not reference a live SQL row");
            }
            const Row row = DeserializeTuple(SqlSchema(), *tuple);
            return "Result: " + std::to_string(std::get<int64_t>(row[1]));
        }
        return "Key " + std::to_string(key) + " not found.";
    }

    return "Unknown command. Use INSERT or SELECT.";
}

std::vector<Row> Executor::ScanWithPredicate(int start_key, int end_key, Predicate pred) {
    std::vector<Row> results;
    std::shared_lock<std::shared_mutex> lock(statement_gate_);
    const auto entries =
        tree_.RangeScan(EncodeIndexKey(start_key), EncodeIndexKey(end_key));
    for (const auto& entry : entries) {
        const std::optional<Tuple> tuple = heap_->Get(RID::Unpack(entry.second));
        if (!tuple) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Index RID does not reference a live SQL row");
        }
        Row row = DeserializeTuple(SqlSchema(), *tuple);
        if (pred(row)) {
            results.push_back(row);
        }
    }
    
    return results;
}

std::string Executor::FormatValue(const DbValue& val) {
    if (std::holds_alternative<std::monostate>(val)) {
        return "NULL";
    }
    if (const auto* value = std::get_if<int32_t>(&val)) {
        return std::to_string(*value);
    }
    if (const auto* value = std::get_if<int64_t>(&val)) {
        return std::to_string(*value);
    }
    if (const auto* value = std::get_if<double>(&val)) {
        return std::to_string(*value);
    }
    if (const auto* value = std::get_if<std::string>(&val)) {
        return "'" + *value + "'";
    }
    return std::get<bool>(val) ? "TRUE" : "FALSE";
}

} // namespace relational_db
