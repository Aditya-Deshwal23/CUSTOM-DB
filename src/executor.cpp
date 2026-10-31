#include "executor.h"

#include <cctype>
#include <charconv>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>

#include "sql_executor.h"
#include "sql_parser.h"

namespace relational_db {
namespace {

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool ParseInt32(std::string_view token, int32_t& value) {
    const char* const first = token.data();
    const char* const last = first + token.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool ParseInt64(std::string_view token, int64_t& value) {
    const char* const first = token.data();
    const char* const last = first + token.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

}  // namespace

Executor::Executor(std::shared_ptr<BufferPoolManager> bpm) : bpm_(std::move(bpm)) {
    tree_ = std::make_unique<BPlusTree<IndexKey16, uint64_t, IndexKey16Less>>(
        bpm_, IndexKey16Less(), IndexKeyKind::INT32);
    InitializeHeap();
    sql_engine_ =
        std::make_unique<SqlExecutionEngine>(bpm_, wal_, statement_gate_, tree_->MetaPageId());
}

Executor::Executor(std::shared_ptr<BufferPoolManager> bpm, WriteAheadLog& wal)
    : bpm_(std::move(bpm)), wal_(&wal) {
    auto transaction = bpm_->BeginTransaction(wal);
    tree_ = std::make_unique<BPlusTree<IndexKey16, uint64_t, IndexKey16Less>>(
        bpm_, IndexKey16Less(), IndexKeyKind::INT32);
    InitializeHeap();
    transaction->Commit();
    sql_engine_ =
        std::make_unique<SqlExecutionEngine>(bpm_, wal_, statement_gate_, tree_->MetaPageId());
}

Executor::~Executor() = default;

const Schema& Executor::SqlSchema() {
    static const Schema schema{
        {Column{"key", ColumnType::INT}, Column{"value", ColumnType::BIGINT}}, std::nullopt};
    return schema;
}

void Executor::InitializeHeap() {
    page_id_t heap_header_page_id = INVALID_PAGE_ID;
    {
        ReadPageGuard meta_guard = bpm_->FetchRead(tree_->MetaPageId());
        const auto* meta = meta_guard.As<std::array<char, PAGE_DATA_SIZE>>();
        heap_header_page_id = bpt_detail::ReadPageId(
            reinterpret_cast<const uint8_t*>(meta->data() + sizeof(bpt_detail::IndexMetaLayout)));
    }

    if (heap_header_page_id == INVALID_PAGE_ID) {
        TableHeap heap = TableHeap::Create(*bpm_);
        heap_header_page_id = heap.HeaderPageId();
        heap_ = std::make_unique<TableHeap>(std::move(heap));
        WritePageGuard meta_guard = bpm_->FetchWrite(tree_->MetaPageId());
        auto* meta = meta_guard.AsMut<std::array<char, PAGE_DATA_SIZE>>();
        bpt_detail::WritePageId(
            reinterpret_cast<uint8_t*>(meta->data() + sizeof(bpt_detail::IndexMetaLayout)),
            heap_header_page_id);
    } else {
        heap_ = std::make_unique<TableHeap>(*bpm_, heap_header_page_id);
    }
}

std::string Executor::ExecuteQuery(const std::string& query) {
    try {
        std::vector<SqlToken> tokens;
        try {
            tokens = SqlLexer::Tokenize(query);
        } catch (const SqlError&) {
            std::string result = sql_engine_->Execute(query);
            bpm_->AssertNoPinsHeld();
            return result;
        }
        size_t content_end = tokens.size() - 1U;
        if (content_end != 0U && tokens[content_end - 1U].kind == SqlTokenKind::SEMICOLON) {
            --content_end;
        }
        if (content_end == 3U && tokens[1].kind == SqlTokenKind::INTEGER &&
            tokens[2].kind == SqlTokenKind::INTEGER && EqualsIgnoreCase(tokens[0].text, "INSERT")) {
            std::string result = ExecuteLegacyQuery(query);
            bpm_->AssertNoPinsHeld();
            return result;
        }
        if (content_end == 2U && tokens[1].kind == SqlTokenKind::INTEGER &&
            EqualsIgnoreCase(tokens[0].text, "SELECT")) {
            std::string result = ExecuteLegacyQuery(query);
            bpm_->AssertNoPinsHeld();
            return result;
        }
        std::string result = sql_engine_->Execute(query);
        bpm_->AssertNoPinsHeld();
        return result;
    } catch (const std::exception&) {
        bpm_->AssertNoPinsHeld();
        throw;
    }
}

SqlExecutionResult Executor::ExecuteResult(const std::string& query,
                                           std::chrono::milliseconds timeout, uint64_t max_rows) {
    SqlExecutionResult result = sql_engine_->ExecuteResult(query, timeout, max_rows);
    bpm_->AssertNoPinsHeld();
    return result;
}

std::vector<SqlTableMetadata> Executor::GetTables() {
    return sql_engine_->GetTables();
}

std::optional<SqlTableMetadata> Executor::GetTable(std::string_view name) {
    return sql_engine_->GetTable(name);
}

std::string Executor::ExecuteLegacyQuery(const std::string& query) {
    const std::vector<SqlToken> tokens = SqlLexer::Tokenize(query);
    const std::string_view command = tokens.size() > 1 ? tokens[0].text : std::string_view{};
    const bool semicolon =
        tokens.size() > 1 && tokens[tokens.size() - 2].kind == SqlTokenKind::SEMICOLON;
    const size_t statement_end = semicolon ? tokens.size() - 2 : tokens.size() - 1;

    if (EqualsIgnoreCase(command, "INSERT")) {
        if (statement_end != 3 || tokens[1].kind != SqlTokenKind::INTEGER ||
            tokens[2].kind != SqlTokenKind::INTEGER) {
            return "Syntax error: INSERT <key> <value>";
        }
        int32_t key = 0;
        int64_t value = 0;
        if (!ParseInt32(tokens[1].text, key) || !ParseInt64(tokens[2].text, value)) {
            return "Syntax error: INSERT <key> <value>";
        }
        StatementScope scope(statement_gate_, true);
        if (wal_ == nullptr) {
            throw DbError(ErrorCode::INVALID_ARGUMENT,
                          "Legacy INSERT requires a WAL-backed Executor");
        }
        const IndexKey16 index_key = EncodeIndexKey(key);
        if (tree_->GetValue(index_key)) {
            throw DbError(ErrorCode::DUPLICATE_KEY, "Duplicate key in SQL index");
        }
        std::shared_ptr<TxnContext> transaction;
        if (wal_ != nullptr) {
            transaction = bpm_->BeginTransaction(*wal_);
        }
        try {
            const Row row{key, value};
            const Tuple tuple = SerializeTuple(SqlSchema(), row);
            const RID rid = heap_->Insert(tuple);
            tree_->Insert(index_key, rid.Pack());
            if (transaction) {
                transaction->Commit();
            }
        } catch (...) {
            if (transaction) {
                transaction->Rollback();
                tree_ = std::make_unique<BPlusTree<IndexKey16, uint64_t, IndexKey16Less>>(
                    bpm_, IndexKey16Less(), IndexKeyKind::INT32);
            }
            throw;
        }
        return "Inserted (" + std::to_string(key) + ", " + std::to_string(value) + ")";
    } else if (EqualsIgnoreCase(command, "SELECT")) {
        if (statement_end != 2 || tokens[1].kind != SqlTokenKind::INTEGER) {
            return "Syntax error: SELECT <key>";
        }
        int32_t key = 0;
        if (!ParseInt32(tokens[1].text, key)) {
            return "Syntax error: SELECT <key>";
        }
        StatementScope scope(statement_gate_, false);
        const std::optional<uint64_t> packed_rid = tree_->GetValue(EncodeIndexKey(key));
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
    StatementScope scope(statement_gate_, false);
    const IndexKey16 high = EncodeIndexKey(end_key);
    IndexKey16Less less;
    auto iterator = tree_->LowerBound(EncodeIndexKey(start_key));
    while (std::optional<std::pair<IndexKey16, uint64_t>> entry = iterator.Next()) {
        scope.Cancellation().Check();
        if (!less(entry->first, high)) {
            break;
        }
        const std::optional<Tuple> tuple = heap_->Get(RID::Unpack(entry->second));
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

std::optional<std::string> Executor::IntegrityCheck() {
    StatementScope scope(statement_gate_, false);
    try {
        bpm_->VerifyDiskPages();
    } catch (const DbError& error) {
        return std::string("Page checksum or format validation failed: ") + error.what();
    }

    if (const std::optional<std::string> error = tree_->Validate()) {
        return error;
    }

    const auto entries =
        tree_->RangeScan(EncodeIndexKey(std::numeric_limits<int32_t>::min()),
                         EncodeIndexKey(std::numeric_limits<int32_t>::max()), true, true);
    for (const auto& entry : entries) {
        const RID rid = RID::Unpack(entry.second);
        const std::optional<Tuple> tuple = heap_->Get(rid);
        if (!tuple) {
            return "B+ tree entry references a missing heap row";
        }
        const Row row = DeserializeTuple(SqlSchema(), *tuple);
        if (std::get<int32_t>(row[0]) != DecodeIndexKeyInt32(entry.first)) {
            return "B+ tree key does not match its heap row";
        }
    }

    size_t heap_rows = 0;
    TableIterator iterator = heap_->Begin();
    RID rid{};
    Tuple tuple;
    while (iterator.Next(rid, tuple)) {
        const Row row = DeserializeTuple(SqlSchema(), tuple);
        const auto indexed = tree_->GetValue(EncodeIndexKey(std::get<int32_t>(row[0])));
        if (!indexed || *indexed != rid.Pack()) {
            return "Heap row is missing from the B+ tree or has a different RID";
        }
        ++heap_rows;
    }
    if (heap_rows != entries.size()) {
        return "Heap and B+ tree row counts differ";
    }
    return std::nullopt;
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

}  // namespace relational_db
