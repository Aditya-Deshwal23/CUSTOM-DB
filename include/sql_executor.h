#ifndef RELDB_SQL_EXECUTOR_H
#define RELDB_SQL_EXECUTOR_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "catalog.h"
#include "concurrency.h"
#include "wal.h"

namespace relational_db {

struct OperatorMetrics {
    std::string name;
    uint64_t rows{0};
    uint64_t elapsed_ns{0};
    uint64_t pages_fetched{0};
    uint64_t pages_missed{0};
};

class OperatorStatsSink {
   public:
    void Add(OperatorMetrics metrics);
    const std::vector<OperatorMetrics>& Metrics() const noexcept {
        return metrics_;
    }

   private:
    std::vector<OperatorMetrics> metrics_;
};

struct ExecutionContext {
    Catalog* catalog{nullptr};
    BufferPoolManager* bpm{nullptr};
    TxnContext* txn{nullptr};
    CancellationToken* cancel{nullptr};
    OperatorStatsSink* stats{nullptr};
};

struct SqlExecutionResult {
    std::string statement;
    std::string message;
    std::vector<Column> columns;
    std::vector<std::string> column_names;
    std::vector<Row> rows;
    uint64_t rows_affected{0};
    bool truncated{false};
    std::string plan;
    std::optional<WalCommitInfo> commit;
};

struct SqlInspectFilter {
    size_t column{0};
    std::string operation;
    Value value;
};

struct SqlInspectedRow {
    RID rid;
    Row values;
};

struct SqlInspectedRows {
    std::vector<SqlInspectedRow> rows;
    std::string next_cursor;
};

struct SqlHeapPageInspection {
    std::string table;
    bool header_page{false};
    std::vector<std::string> column_names;
    std::vector<SqlInspectedRow> rows;
};

struct SqlIndexPageOwner {
    std::string name;
    std::string column;
    ColumnType column_type{ColumnType::INT};
};

struct SqlIndexTreeInspection {
    SqlIndexTree::InspectionTree tree;
    ColumnType column_type{ColumnType::INT};
};

struct InspectedPage {
    page_id_t page_id{INVALID_PAGE_ID};
    PageType type{PageType::HEAP};
    uint16_t flags{0};
    uint64_t lsn{0};
    bool crc_ok{false};
    std::string read_error;
    std::array<char, PAGE_DATA_SIZE> body{};
};

struct SqlIndexMetadata {
    std::string name;
    std::string column;
    ColumnType column_type{ColumnType::INT};
    bool unique{false};
    page_id_t meta_page_id{INVALID_PAGE_ID};
    uint16_t height{0};
    uint64_t pages{0};
    uint64_t entries{0};
    uint64_t leaf_pages{0};
    double fill_factor{0.0};
};

struct SqlTableMetadata {
    std::string name;
    Schema schema;
    std::vector<bool> not_null;
    uint64_t row_estimate{0};
    TableHeapInspection heap;
    uint64_t inspection_pages{0};
    std::vector<SqlIndexMetadata> indexes;
};

class VolcanoOperator {
   public:
    explicit VolcanoOperator(ExecutionContext& context);
    virtual ~VolcanoOperator() = default;

    void Init();
    bool Next(Tuple* tuple, RID* rid);
    void Finish();
    virtual std::string Explain(size_t depth = 0) const = 0;

   protected:
    virtual void InitImpl() = 0;
    virtual bool NextImpl(Tuple* tuple, RID* rid) = 0;
    virtual void FinishImpl();
    virtual std::string Name() const = 0;
    ExecutionContext& context_;

   private:
};

class SqlExecutionEngine {
   public:
    SqlExecutionEngine(std::shared_ptr<BufferPoolManager> buffer_pool, WriteAheadLog* wal,
                       WriterPreferringSharedMutex& statement_gate, page_id_t owner_meta_page_id);
    ~SqlExecutionEngine();
    SqlExecutionEngine(const SqlExecutionEngine&) = delete;
    SqlExecutionEngine& operator=(const SqlExecutionEngine&) = delete;

    std::string Execute(
        std::string_view query,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(STATEMENT_GATE_TIMEOUT_MS));
    SqlExecutionResult ExecuteResult(std::string_view query, std::chrono::milliseconds timeout,
                                     uint64_t max_rows,
                                     CancellationToken* external_cancellation = nullptr);
    std::vector<SqlExecutionResult> ExecuteBatch(const std::vector<std::string>& queries,
                                                 std::chrono::milliseconds timeout,
                                                 uint64_t max_rows,
                                                 size_t& failed_statement_index,
                                                 CancellationToken* external_cancellation = nullptr);
    std::vector<SqlTableMetadata> GetTables(uint64_t max_pages);
    std::optional<SqlTableMetadata> GetTable(std::string_view name);
    SqlIndexTreeInspection InspectIndexTree(std::string_view name, uint64_t max_nodes);
    std::pair<SqlIndexTree::LookupTraceResult, std::vector<SqlIndexTree::TraceDecision>> TraceIndex(
        std::string_view name, std::string_view key);
    SqlInspectedRows InspectRows(std::string_view table, uint64_t limit, std::string_view after,
                                 size_t order_column, bool descending,
                                 const std::vector<SqlInspectFilter>& filters, uint64_t max_pages,
                                 std::chrono::milliseconds timeout);
    std::optional<SqlHeapPageInspection> InspectHeapPage(page_id_t page_id, uint64_t max_pages);
    std::optional<SqlIndexPageOwner> FindIndexPageOwner(page_id_t page_id, uint64_t max_pages);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace relational_db

#endif
