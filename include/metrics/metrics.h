#ifndef RELDB_METRICS_METRICS_H
#define RELDB_METRICS_METRICS_H

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace relational_db {

enum class HttpRoute : size_t {
    UNKNOWN,
    HEALTH,
    QUERY,
    LEGACY_QUERY,
    API_METRICS,
    PROMETHEUS,
    STATS,
    BUFFER_POOL_STATS,
    STORAGE_STATS,
    OTHER,
    COUNT
};

enum class StatementMetric : size_t { SELECT, INSERT, UPDATE, DELETE, DDL, COUNT };

enum class MetricError : size_t {
    UNKNOWN,
    INVALID_DB_FILE,
    UNSUPPORTED_VERSION,
    HEADER_CORRUPT,
    PAGE_CHECKSUM_MISMATCH,
    PAGE_OUT_OF_RANGE,
    PAGE_CORRUPT,
    BUFFER_POOL_EXHAUSTED,
    INVALID_ARGUMENT,
    INVALID_TUPLE,
    TUPLE_TOO_LARGE,
    SYNTAX_ERROR,
    UNKNOWN_TABLE,
    UNKNOWN_COLUMN,
    TYPE_MISMATCH,
    DUPLICATE_KEY,
    NOT_NULL_VIOLATION,
    FK_VIOLATION,
    TXN_TOO_LARGE,
    BUSY,
    QUERY_TIMEOUT,
    RESULT_LIMIT_EXCEEDED,
    SORT_LIMIT_EXCEEDED,
    IO_ERROR,
    STORAGE_QUOTA,
    RATE_LIMITED,
    API_KEY_REQUIRED,
    READ_ONLY,
    OTHER,
    COUNT
};

MetricError MetricErrorFromCode(std::string_view code) noexcept;
bool MetricUpdatesEnabled() noexcept;
void SetMetricUpdatesEnabled(bool enabled) noexcept;

class Counter {
   public:
    void Increment() noexcept {
        Add(1);
    }

    void Add(uint64_t amount) noexcept {
        if (MetricUpdatesEnabled()) {
            value_.fetch_add(amount, std::memory_order_relaxed);
        }
    }

    uint64_t Value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<uint64_t> value_{0};
};

class Gauge {
   public:
    void Set(int64_t value) noexcept {
        if (MetricUpdatesEnabled()) {
            value_.store(value, std::memory_order_relaxed);
        }
    }

    void Add(int64_t amount) noexcept {
        if (MetricUpdatesEnabled()) {
            value_.fetch_add(amount, std::memory_order_relaxed);
        }
    }

    int64_t Value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<int64_t> value_{0};
};

struct HistogramSnapshot {
    static constexpr size_t kBucketCount = 32;

    std::array<uint64_t, kBucketCount> buckets{};
    uint64_t sum{0};
    uint64_t count{0};

    uint64_t Quantile(double quantile) const noexcept {
        if (count == 0 || !std::isfinite(quantile)) {
            return 0;
        }
        if (quantile <= 0.0) {
            quantile = 0.0;
        } else if (quantile >= 1.0) {
            quantile = 1.0;
        }
        const uint64_t rank = static_cast<uint64_t>(
            std::ceil(quantile * static_cast<double>(count)));
        uint64_t cumulative = 0;
        for (size_t index = 0; index < buckets.size(); ++index) {
            cumulative += buckets[index];
            if (cumulative >= rank) {
                return uint64_t{1} << index;
            }
        }
        return uint64_t{1} << (buckets.size() - 1U);
    }

    uint64_t P50() const noexcept {
        return Quantile(0.50);
    }

    uint64_t P95() const noexcept {
        return Quantile(0.95);
    }

    uint64_t P99() const noexcept {
        return Quantile(0.99);
    }
};

class Histogram {
   public:
    static constexpr size_t kBucketCount = HistogramSnapshot::kBucketCount;

    void Observe(uint64_t microseconds) noexcept {
        if (!MetricUpdatesEnabled()) {
            return;
        }
        size_t bucket = 0;
        uint64_t upper_bound = 1;
        while (microseconds > upper_bound && bucket + 1U < kBucketCount) {
            upper_bound <<= 1U;
            ++bucket;
        }
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(microseconds, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    HistogramSnapshot Snapshot() const noexcept {
        HistogramSnapshot snapshot;
        for (size_t index = 0; index < buckets_.size(); ++index) {
            snapshot.buckets[index] = buckets_[index].load(std::memory_order_relaxed);
        }
        snapshot.sum = sum_.load(std::memory_order_relaxed);
        snapshot.count = count_.load(std::memory_order_relaxed);
        return snapshot;
    }

   private:
    std::array<std::atomic<uint64_t>, kBucketCount> buckets_{};
    std::atomic<uint64_t> sum_{0};
    std::atomic<uint64_t> count_{0};
};

template <size_t LabelCount>
class LabeledCounter {
   public:
    explicit LabeledCounter(std::array<std::string_view, LabelCount> labels) noexcept
        : labels_(labels) {}

    Counter& ForLabel(size_t index) noexcept {
        return counters_[index];
    }

    const Counter& ForLabel(size_t index) const noexcept {
        return counters_[index];
    }

    std::string_view Label(size_t index) const noexcept {
        return labels_[index];
    }

   private:
    std::array<std::string_view, LabelCount> labels_;
    std::array<Counter, LabelCount> counters_{};
};

template <size_t LabelCount>
class LabeledHistogram {
   public:
    explicit LabeledHistogram(std::array<std::string_view, LabelCount> labels) noexcept
        : labels_(labels) {}

    Histogram& ForLabel(size_t index) noexcept {
        return histograms_[index];
    }

    const Histogram& ForLabel(size_t index) const noexcept {
        return histograms_[index];
    }

    std::string_view Label(size_t index) const noexcept {
        return labels_[index];
    }

   private:
    std::array<std::string_view, LabelCount> labels_;
    std::array<Histogram, LabelCount> histograms_{};
};

struct MetricsRegistry {
    static constexpr std::array<std::string_view, 10> kHttpRouteLabels{
        "unknown", "/health", "/api/query", "/api/v1/query", "/api/metrics",
        "/metrics", "/api/v1/stats", "/api/v1/stats/buffer_pool",
        "/api/v1/stats/storage", "other"};
    static constexpr std::array<std::string_view, 16> kHttpStatusLabels{
        "200", "201", "204", "400", "401", "403", "404", "409",
        "413", "415", "422", "429", "500", "503", "507", "other"};
    static constexpr std::array<std::string_view, 5> kStatementLabels{
        "select", "insert", "update", "delete", "ddl"};
    static constexpr std::array<std::string_view, 29> kErrorLabels{
        "unknown", "invalid_db_file", "unsupported_version", "header_corrupt",
        "page_checksum_mismatch", "page_out_of_range", "page_corrupt",
        "buffer_pool_exhausted", "invalid_argument", "invalid_tuple", "tuple_too_large",
        "syntax_error", "unknown_table", "unknown_column", "type_mismatch", "duplicate_key",
        "not_null_violation", "foreign_key_violation", "txn_too_large", "busy",
        "query_timeout", "result_limit_exceeded", "sort_limit_exceeded", "io_error",
        "storage_quota", "rate_limited", "api_key_required", "read_only", "other"};

    static MetricsRegistry& Instance() noexcept {
        static MetricsRegistry registry;
        return registry;
    }

    void RecordHttpRequest(HttpRoute route, int status) noexcept;
    void RecordStatement(StatementMetric statement, uint64_t elapsed_us) noexcept;
    void RecordError(MetricError error) noexcept;
    uint64_t ErrorCount() const noexcept;

    std::array<std::array<Counter, kHttpStatusLabels.size()>, kHttpRouteLabels.size()>
        http_requests{};
    std::array<Counter, kStatementLabels.size()> statements{};
    std::array<Histogram, kStatementLabels.size()> statement_duration_us{};
    std::array<Counter, kErrorLabels.size()> errors{};
    Counter rows_read;
    Counter rows_written;
    Counter index_scans;
    Counter sequential_scans;
    Counter rate_limited_requests;
    Counter busy_rejections;
    Counter wal_bytes;
    Counter wal_commits;
    Counter checkpoints;
    Counter recovery_records;
    Histogram gate_wait_us;
    Histogram http_request_duration_us;
    Histogram disk_read_us;
    Histogram disk_write_us;
    Histogram disk_sync_us;
    Histogram wal_sync_us;
    Histogram checkpoint_duration_us;
    Histogram recovery_duration_us;
    Gauge active_statements;
    Gauge statement_waiters;
    Gauge demo_load_active;
    Gauge recovery_ran;

   private:
    static size_t StatusIndex(int status) noexcept;
};

}  // namespace relational_db

#endif
