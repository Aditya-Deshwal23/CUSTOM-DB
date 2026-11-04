#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "b_plus_tree.h"
#include "concurrency.h"
#include "sql_executor.h"
#include "storage/table_heap.h"
#include "storage/value.h"

namespace relational_db {

class SqlExecutionEngine;

using DbValue = Value;
using Predicate = std::function<bool(const Row&)>;

class Executor {
   public:
    explicit Executor(std::shared_ptr<BufferPoolManager> bpm);
    Executor(std::shared_ptr<BufferPoolManager> bpm, WriteAheadLog& wal);
    ~Executor();

    /**
     * @brief Executes a simple string-based query.
     * Backported to use std::string for compatibility.
     */
    std::string ExecuteQuery(const std::string& query);
    SqlExecutionResult ExecuteResult(const std::string& query, std::chrono::milliseconds timeout,
                                     uint64_t max_rows = MAX_RESULT_ROWS,
                                     CancellationToken* external_cancellation = nullptr);
    std::vector<SqlExecutionResult> ExecuteBatch(const std::vector<std::string>& queries,
                                                 std::chrono::milliseconds timeout,
                                                 uint64_t max_rows,
                                                 size_t& failed_statement_index,
                                                 CancellationToken* external_cancellation = nullptr);
    std::vector<SqlTableMetadata> GetTables(uint64_t max_pages = 4096);
    std::optional<SqlTableMetadata> GetTable(std::string_view name);
    std::vector<InspectedPage> InspectPages(page_id_t from, uint64_t count);
    SqlIndexTreeInspection InspectIndexTree(std::string_view name, uint64_t max_nodes);
    std::pair<SqlIndexTree::LookupTraceResult, std::vector<SqlIndexTree::TraceDecision>> TraceIndex(
        std::string_view name, std::string_view key);
    SqlInspectedRows InspectRows(std::string_view table, uint64_t limit, std::string_view after,
                                 size_t order_column, bool descending,
                                 const std::vector<SqlInspectFilter>& filters, uint64_t max_pages,
                                 std::chrono::milliseconds timeout);
    std::optional<SqlHeapPageInspection> InspectHeapPage(page_id_t page_id, uint64_t max_pages);
    std::optional<SqlIndexPageOwner> FindIndexPageOwner(page_id_t page_id, uint64_t max_pages);
    std::unique_ptr<StatementScope> AcquireInspectionGate();
    WalInspectionSnapshot InspectWalRecords(size_t limit,
                                            std::optional<uint64_t> before_lsn = std::nullopt);
    WriteAheadLog* Wal() const noexcept {
        return wal_;
    }

    std::vector<Row> ScanWithPredicate(int start_key, int end_key, Predicate pred);
    std::optional<std::string> IntegrityCheck();

    static std::string FormatValue(const DbValue& val);

   private:
    static const Schema& SqlSchema();
    void InitializeHeap();
    std::string ExecuteLegacyQuery(const std::string& query);

    std::shared_ptr<BufferPoolManager> bpm_;
    mutable WriterPreferringSharedMutex statement_gate_;
    WriteAheadLog* wal_{nullptr};
    std::unique_ptr<BPlusTree<IndexKey16, uint64_t, IndexKey16Less>> tree_;
    std::unique_ptr<TableHeap> heap_;
    std::unique_ptr<SqlExecutionEngine> sql_engine_;
};

}  // namespace relational_db

#endif  // EXECUTOR_H
