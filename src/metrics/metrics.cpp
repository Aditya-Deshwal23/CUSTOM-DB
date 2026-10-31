#include "metrics/metrics.h"

#include <atomic>

namespace relational_db {
namespace {

std::atomic<bool> metric_updates_enabled{true};

}  // namespace

bool MetricUpdatesEnabled() noexcept {
    return metric_updates_enabled.load(std::memory_order_relaxed);
}

void SetMetricUpdatesEnabled(bool enabled) noexcept {
    metric_updates_enabled.store(enabled, std::memory_order_relaxed);
}

MetricError MetricErrorFromCode(std::string_view code) noexcept {
    if (code == "INVALID_ARGUMENT" || code == "INVALID_JSON" || code == "INVALID_REQUEST" ||
        code == "INVALID_PARAMS" || code == "SYNTAX_ERROR" || code == "BODY_TOO_LARGE" ||
        code == "SQL_TOO_LARGE" || code == "UNSUPPORTED_MEDIA_TYPE" ||
        code == "INVALID_CONTENT_LENGTH") {
        return code == "SYNTAX_ERROR" ? MetricError::SYNTAX_ERROR
                                      : MetricError::INVALID_ARGUMENT;
    }
    if (code == "INVALID_DB_FILE") {
        return MetricError::INVALID_DB_FILE;
    }
    if (code == "UNSUPPORTED_VERSION") {
        return MetricError::UNSUPPORTED_VERSION;
    }
    if (code == "HEADER_CORRUPT") {
        return MetricError::HEADER_CORRUPT;
    }
    if (code == "PAGE_CHECKSUM_MISMATCH") {
        return MetricError::PAGE_CHECKSUM_MISMATCH;
    }
    if (code == "PAGE_OUT_OF_RANGE") {
        return MetricError::PAGE_OUT_OF_RANGE;
    }
    if (code == "PAGE_CORRUPT") {
        return MetricError::PAGE_CORRUPT;
    }
    if (code == "BUFFER_POOL_EXHAUSTED") {
        return MetricError::BUFFER_POOL_EXHAUSTED;
    }
    if (code == "INVALID_TUPLE") {
        return MetricError::INVALID_TUPLE;
    }
    if (code == "TUPLE_TOO_LARGE") {
        return MetricError::TUPLE_TOO_LARGE;
    }
    if (code == "UNKNOWN_TABLE") {
        return MetricError::UNKNOWN_TABLE;
    }
    if (code == "UNKNOWN_COLUMN") {
        return MetricError::UNKNOWN_COLUMN;
    }
    if (code == "TYPE_MISMATCH") {
        return MetricError::TYPE_MISMATCH;
    }
    if (code == "DUPLICATE_KEY") {
        return MetricError::DUPLICATE_KEY;
    }
    if (code == "NOT_NULL_VIOLATION") {
        return MetricError::NOT_NULL_VIOLATION;
    }
    if (code == "FK_VIOLATION") {
        return MetricError::FK_VIOLATION;
    }
    if (code == "TXN_TOO_LARGE") {
        return MetricError::TXN_TOO_LARGE;
    }
    if (code == "BUSY" || code == "UNAVAILABLE") {
        return MetricError::BUSY;
    }
    if (code == "QUERY_TIMEOUT") {
        return MetricError::QUERY_TIMEOUT;
    }
    if (code == "RESULT_LIMIT_EXCEEDED") {
        return MetricError::RESULT_LIMIT_EXCEEDED;
    }
    if (code == "SORT_LIMIT_EXCEEDED") {
        return MetricError::SORT_LIMIT_EXCEEDED;
    }
    if (code == "IO_ERROR" || code == "INTERNAL") {
        return MetricError::IO_ERROR;
    }
    if (code == "STORAGE_QUOTA") {
        return MetricError::STORAGE_QUOTA;
    }
    if (code == "RATE_LIMITED") {
        return MetricError::RATE_LIMITED;
    }
    if (code == "API_KEY_REQUIRED") {
        return MetricError::API_KEY_REQUIRED;
    }
    if (code == "READ_ONLY") {
        return MetricError::READ_ONLY;
    }
    return MetricError::OTHER;
}

size_t MetricsRegistry::StatusIndex(int status) noexcept {
    switch (status) {
        case 200:
            return 0;
        case 201:
            return 1;
        case 204:
            return 2;
        case 400:
            return 3;
        case 401:
            return 4;
        case 403:
            return 5;
        case 404:
            return 6;
        case 409:
            return 7;
        case 413:
            return 8;
        case 415:
            return 9;
        case 422:
            return 10;
        case 429:
            return 11;
        case 500:
            return 12;
        case 503:
            return 13;
        case 507:
            return 14;
        default:
            return 15;
    }
}

void MetricsRegistry::RecordHttpRequest(HttpRoute route, int status) noexcept {
    const size_t route_index = static_cast<size_t>(route);
    const size_t safe_route = route_index < kHttpRouteLabels.size() ? route_index : 0U;
    http_requests[safe_route][StatusIndex(status)].Increment();
}

void MetricsRegistry::RecordStatement(StatementMetric statement, uint64_t elapsed_us) noexcept {
    const size_t index = static_cast<size_t>(statement);
    if (index >= kStatementLabels.size()) {
        return;
    }
    statements[index].Increment();
    statement_duration_us[index].Observe(elapsed_us);
}

void MetricsRegistry::RecordError(MetricError error) noexcept {
    const size_t index = static_cast<size_t>(error);
    errors[index < kErrorLabels.size() ? index : kErrorLabels.size() - 1U].Increment();
}

uint64_t MetricsRegistry::ErrorCount() const noexcept {
    uint64_t count = 0;
    for (const Counter& error : errors) {
        count += error.Value();
    }
    return count;
}

}  // namespace relational_db
