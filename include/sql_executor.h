#ifndef RELDB_SQL_EXECUTOR_H
#define RELDB_SQL_EXECUTOR_H

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
    std::vector<Column> columns;
    std::vector<std::string> column_names;
    std::vector<Row> rows;
    uint64_t rows_affected{0};
    std::string plan;
};

struct SqlIndexMetadata {
    std::string name;
    std::string column;
    bool unique{false};
};

struct SqlTableMetadata {
    std::string name;
    Schema schema;
    uint64_t row_estimate{0};
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
                                     uint64_t max_rows);
    std::vector<SqlTableMetadata> GetTables();
    std::optional<SqlTableMetadata> GetTable(std::string_view name);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace relational_db

#endif
