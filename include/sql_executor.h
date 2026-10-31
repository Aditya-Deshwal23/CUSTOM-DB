#ifndef RELDB_SQL_EXECUTOR_H
#define RELDB_SQL_EXECUTOR_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "catalog.h"
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

class CancellationToken {
   public:
    explicit CancellationToken(std::chrono::steady_clock::time_point deadline =
                                   std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(STATEMENT_TIMEOUT_MS));

    void Cancel() noexcept;
    void Check() const;

   private:
    std::atomic<bool> cancelled_{false};
    std::chrono::steady_clock::time_point deadline_;
};

struct ExecutionContext {
    Catalog* catalog{nullptr};
    BufferPoolManager* bpm{nullptr};
    TxnContext* txn{nullptr};
    CancellationToken* cancel{nullptr};
    OperatorStatsSink* stats{nullptr};
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
                       std::shared_mutex& statement_gate, page_id_t owner_meta_page_id);
    ~SqlExecutionEngine();
    SqlExecutionEngine(const SqlExecutionEngine&) = delete;
    SqlExecutionEngine& operator=(const SqlExecutionEngine&) = delete;

    std::string Execute(std::string_view query);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace relational_db

#endif
