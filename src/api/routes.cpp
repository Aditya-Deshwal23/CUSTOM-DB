#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include "internal.h"
#include "metrics/metrics.h"

#if defined(__linux__) || defined(__APPLE__)
#include <dirent.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace relational_db {
namespace {

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::string Base64Encode(const std::vector<uint8_t>& bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (size_t index = 0; index < bytes.size(); index += 3U) {
        const uint32_t first = bytes[index];
        const uint32_t second = index + 1U < bytes.size() ? bytes[index + 1U] : 0U;
        const uint32_t third = index + 2U < bytes.size() ? bytes[index + 2U] : 0U;
        const uint32_t group = (first << 16U) | (second << 8U) | third;
        encoded.push_back(alphabet[(group >> 18U) & 0x3fU]);
        encoded.push_back(alphabet[(group >> 12U) & 0x3fU]);
        encoded.push_back(index + 1U < bytes.size() ? alphabet[(group >> 6U) & 0x3fU] : '=');
        encoded.push_back(index + 2U < bytes.size() ? alphabet[group & 0x3fU] : '=');
    }
    return encoded;
}

uint64_t UnixTimeMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

std::string RoundTripDdl(const std::string& ddl) {
    std::string round_tripped;
    size_t begin = 0;
    while (begin < ddl.size()) {
        const size_t end = ddl.find(';', begin);
        const size_t length = end == std::string::npos ? ddl.size() - begin : end - begin;
        const std::string_view statement(ddl.data() + begin, length);
        try {
            round_tripped += PrintSql(SqlParser(statement).Parse());
        } catch (const SqlError& error) {
            throw DbError(ErrorCode::INVALID_ARGUMENT,
                          "Catalog DDL did not round-trip: " + std::string(statement) + " (" +
                              error.what() + ")");
        }
        round_tripped += ';';
        begin = end == std::string::npos ? ddl.size() : end + 1U;
    }
    return round_tripped;
}

ApiMiddleware::context& RequestContext(crow::App<ApiMiddleware>& app,
                                       const crow::request& request) {
    return app.get_context<ApiMiddleware>(request);
}

bool IsWrite(SqlStatementKind kind) {
    return kind != SqlStatementKind::SELECT && kind != SqlStatementKind::EXPLAIN &&
           kind != SqlStatementKind::SHOW_TABLES && kind != SqlStatementKind::DESCRIBE;
}

bool IsSafeQueryRequestId(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_' ||
                      character == '.';
           });
}

class ActiveQueryRegistration {
   public:
    ActiveQueryRegistration(ApiServer::Impl& server, const std::string& request_id,
                            std::string_view statement, std::chrono::milliseconds timeout)
        : server_(server), request_id_(request_id) {
        cancellation_ = std::make_shared<CancellationToken>(
            std::chrono::steady_clock::now() + timeout);
        std::lock_guard<std::mutex> lock(server_.active_queries_mutex);
        if (server_.active_queries.size() >= 100U ||
            server_.active_queries.find(request_id_) != server_.active_queries.end()) {
            cancellation_.reset();
            return;
        }
        ActiveQuery query;
        query.request_id = request_id_;
        query.sql_preview = std::string(statement) + " statement";
        query.started = std::chrono::steady_clock::now();
        query.cancellation = cancellation_;
        server_.active_queries.emplace(request_id_, std::move(query));
    }

    ~ActiveQueryRegistration() {
        if (!cancellation_) return;
        std::lock_guard<std::mutex> lock(server_.active_queries_mutex);
        server_.active_queries.erase(request_id_);
    }

    ActiveQueryRegistration(const ActiveQueryRegistration&) = delete;
    ActiveQueryRegistration& operator=(const ActiveQueryRegistration&) = delete;

    CancellationToken* Cancellation() const noexcept {
        return cancellation_.get();
    }

   private:
    ApiServer::Impl& server_;
    const std::string& request_id_;
    std::shared_ptr<CancellationToken> cancellation_;
};

std::string ErrorCodeName(const DbError& error) {
    switch (error.Code()) {
        case ErrorCode::INVALID_DB_FILE:
            return "INVALID_DB_FILE";
        case ErrorCode::UNSUPPORTED_VERSION:
            return "UNSUPPORTED_VERSION";
        case ErrorCode::HEADER_CORRUPT:
            return "HEADER_CORRUPT";
        case ErrorCode::PAGE_CHECKSUM_MISMATCH:
            return "PAGE_CHECKSUM_MISMATCH";
        case ErrorCode::PAGE_OUT_OF_RANGE:
            return "PAGE_OUT_OF_RANGE";
        case ErrorCode::PAGE_CORRUPT:
            return "PAGE_CORRUPT";
        case ErrorCode::BUFFER_POOL_EXHAUSTED:
            return "BUFFER_POOL_EXHAUSTED";
        case ErrorCode::UNKNOWN_TABLE:
            return "UNKNOWN_TABLE";
        case ErrorCode::UNKNOWN_COLUMN:
            return "UNKNOWN_COLUMN";
        case ErrorCode::TYPE_MISMATCH:
            return "TYPE_MISMATCH";
        case ErrorCode::DUPLICATE_KEY:
            return "DUPLICATE_KEY";
        case ErrorCode::NOT_NULL_VIOLATION:
            return "NOT_NULL_VIOLATION";
        case ErrorCode::FK_VIOLATION:
            return "FK_VIOLATION";
        case ErrorCode::BUSY:
            return "BUSY";
        case ErrorCode::INVALID_TUPLE:
            return "INVALID_TUPLE";
        case ErrorCode::TUPLE_TOO_LARGE:
            return "TUPLE_TOO_LARGE";
        case ErrorCode::TXN_TOO_LARGE:
            return "TXN_TOO_LARGE";
        case ErrorCode::QUERY_TIMEOUT:
            return "QUERY_TIMEOUT";
        case ErrorCode::RESULT_LIMIT_EXCEEDED:
            return "RESULT_LIMIT_EXCEEDED";
        case ErrorCode::SORT_LIMIT_EXCEEDED:
            return "SORT_LIMIT_EXCEEDED";
        case ErrorCode::IO_ERROR:
            return "IO_ERROR";
        case ErrorCode::STORAGE_QUOTA:
            return "STORAGE_QUOTA";
        case ErrorCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        default:
            return "INTERNAL";
    }
}

void StoreSlowQuery(ApiServer::Impl& server, const std::string& request_id,
                    std::string_view statement, uint64_t elapsed_us, std::string_view sql,
                    std::string_view plan) {
    SlowQuerySample sample;
    sample.ts_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
    sample.elapsed_us = elapsed_us;
    sample.statement = statement;
    sample.sql_preview = std::string(sql.substr(0, 200U));
    sample.plan_summary = std::string(plan.substr(0, 200U));
    sample.request_id = request_id;
    std::lock_guard<std::mutex> lock(server.slow_queries_mutex);
    server.slow_queries.push_back(std::move(sample));
    while (server.slow_queries.size() > 100U) {
        server.slow_queries.pop_front();
    }
}

int HttpStatusFor(const DbError& error) {
    switch (error.Code()) {
        case ErrorCode::UNKNOWN_TABLE:
            return 404;
        case ErrorCode::DUPLICATE_KEY:
        case ErrorCode::NOT_NULL_VIOLATION:
        case ErrorCode::FK_VIOLATION:
            return 409;
        case ErrorCode::BUSY:
        case ErrorCode::BUFFER_POOL_EXHAUSTED:
        case ErrorCode::QUERY_TIMEOUT:
            return 503;
        case ErrorCode::STORAGE_QUOTA:
            return 507;
        case ErrorCode::INVALID_ARGUMENT:
            return 422;
        case ErrorCode::TYPE_MISMATCH:
            return 400;
        case ErrorCode::UNKNOWN_COLUMN:
            return 422;
        case ErrorCode::RESULT_LIMIT_EXCEEDED:
        case ErrorCode::SORT_LIMIT_EXCEEDED:
            return 413;
        default:
            return 500;
    }
}

int HttpStatusFor(const SqlError& error) {
    switch (error.Code()) {
        case SqlErrorCode::UNKNOWN_TABLE:
            return 404;
        case SqlErrorCode::DUPLICATE_KEY:
        case SqlErrorCode::NOT_NULL_VIOLATION:
        case SqlErrorCode::FK_VIOLATION:
            return 409;
        case SqlErrorCode::SYNTAX_ERROR:
        case SqlErrorCode::TYPE_MISMATCH:
        case SqlErrorCode::INVALID_ARGUMENT:
            return 400;
        case SqlErrorCode::UNKNOWN_COLUMN:
            return 422;
    }
    return 400;
}

crow::response ServeStatic(ApiServer::Impl& server, const crow::request& request,
                           const std::string& file, const std::string& content_type) {
    auto& context = RequestContext(server.app, request);
    const auto found = server.static_files.find(file);
    if (found == server.static_files.end()) {
        return ErrorResponse(404, "NOT_FOUND", "Static file not found", context.request_id);
    }
    const std::string etag = "\"" + std::to_string(std::hash<std::string>{}(found->second)) + "\"";
    if (request.get_header_value("If-None-Match") == etag) {
        crow::response response(304);
        response.add_header("ETag", etag);
        response.add_header("Cache-Control", "public, max-age=60");
        return response;
    }
    crow::response response(found->second);
    response.add_header("Content-Type", content_type);
    response.add_header("ETag", etag);
    response.add_header("Cache-Control", "public, max-age=60");
    return response;
}

crow::response HandlePreflight(ApiServer::Impl& server, const crow::request& request) {
    auto& context = RequestContext(server.app, request);
    const std::string origin = request.get_header_value("Origin");
    const std::string method = request.get_header_value("Access-Control-Request-Method");
    if (std::find(server.config.cors_origins.begin(), server.config.cors_origins.end(), origin) ==
            server.config.cors_origins.end() ||
        (method != "GET" && method != "POST" && method != "OPTIONS")) {
        return ErrorResponse(403, "CORS_ORIGIN_DENIED", "CORS preflight is not allowed",
                             context.request_id);
    }

    crow::response response(204);
    response.add_header("Access-Control-Allow-Origin", origin);
    response.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.add_header("Access-Control-Allow-Headers", "Content-Type, X-API-Key, X-Request-Id");
    response.add_header("Access-Control-Max-Age", "600");
    response.add_header("Vary", "Origin");
    return response;
}

std::string SqlErrorCodeName(const SqlError& error) {
    switch (error.Code()) {
        case SqlErrorCode::SYNTAX_ERROR:
            return "SYNTAX_ERROR";
        case SqlErrorCode::UNKNOWN_TABLE:
            return "UNKNOWN_TABLE";
        case SqlErrorCode::UNKNOWN_COLUMN:
            return "UNKNOWN_COLUMN";
        case SqlErrorCode::TYPE_MISMATCH:
            return "TYPE_MISMATCH";
        case SqlErrorCode::DUPLICATE_KEY:
            return "DUPLICATE_KEY";
        case SqlErrorCode::NOT_NULL_VIOLATION:
            return "NOT_NULL_VIOLATION";
        case SqlErrorCode::FK_VIOLATION:
            return "FK_VIOLATION";
        case SqlErrorCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
    }
    return "SYNTAX_ERROR";
}

crow::json::wvalue JsonValue(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        return crow::json::wvalue(nullptr);
    }
    if (const auto* integer = std::get_if<int32_t>(&value)) {
        return crow::json::wvalue(*integer);
    }
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        return crow::json::wvalue(static_cast<int64_t>(*integer));
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return crow::json::wvalue(*number);
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return crow::json::wvalue(*boolean);
    }
    return crow::json::wvalue(std::get<std::string>(value));
}

std::string ColumnTypeName(ColumnType type) {
    switch (type) {
        case ColumnType::INT:
            return "INT";
        case ColumnType::BIGINT:
            return "BIGINT";
        case ColumnType::DOUBLE:
            return "DOUBLE";
        case ColumnType::BOOLEAN:
            return "BOOLEAN";
        case ColumnType::VARCHAR:
            return "VARCHAR";
    }
    return "UNKNOWN";
}

crow::response HandleQuery(ApiServer::Impl& server, const crow::request& request, bool legacy) {
    ApiMiddleware& api_middleware = server.app.get_middleware<ApiMiddleware>();
    api_middleware.queries.fetch_add(1, std::memory_order_relaxed);
    ApiMiddleware::context& context = RequestContext(server.app, request);
    const std::string& request_id = context.request_id;
    const auto fail = [legacy, &request_id](int status, const std::string& code,
                                            const std::string& message, size_t line = 0,
                                            size_t column = 0, const std::string& hint = {}) {
        crow::response response;
        if (legacy) {
            crow::json::wvalue legacy_response;
            legacy_response["status"] = "error";
            legacy_response["code"] = code;
            legacy_response["message"] = message;
            legacy_response["deprecated"] = true;
            response = JsonResponse(status, legacy_response);
        } else {
            response = ErrorResponse(status, code, message, request_id, line, column, hint);
        }
        if (status == 503) {
            response.add_header("Retry-After", "1");
        }
        if (legacy) {
            MetricsRegistry& metrics = MetricsRegistry::Instance();
            metrics.RecordError(MetricErrorFromCode(code));
            if (code == "BUSY" || code == "BUFFER_POOL_EXHAUSTED") {
                metrics.busy_rejections.Increment();
            }
        }
        return response;
    };
    crow::json::rvalue input = crow::json::load(request.body);
    if (!input || input.t() != crow::json::type::Object) {
        return fail(400, "INVALID_JSON", "Request body must be a JSON object");
    }

    const char* sql_field = "sql";
    bool using_legacy_field = false;
    if (!input.has("sql") && input.has("query")) {
        sql_field = "query";
        using_legacy_field = true;
    }
    if (!input.has(sql_field) || input[sql_field].t() != crow::json::type::String) {
        return fail(400, "INVALID_REQUEST", "sql must be a string");
    }

    const std::string sql = input[sql_field].s();
    if (sql.empty() || sql.size() > server.config.max_sql_bytes) {
        return fail(sql.empty() ? 400 : 413, sql.empty() ? "INVALID_REQUEST" : "SQL_TOO_LARGE",
                    sql.empty() ? "sql must not be empty" : "SQL exceeds MAX_SQL_BYTES");
    }

    crow::json::rvalue empty_params = crow::json::load("[]");
    const crow::json::rvalue& params = input.has("params") ? input["params"] : empty_params;
    std::string bound_sql;
    try {
        bound_sql = BindParameters(sql, params);
    } catch (const std::invalid_argument& error) {
        return fail(400, "INVALID_PARAMS", error.what());
    } catch (const SqlError& error) {
        return fail(400, "SYNTAX_ERROR", error.what(), error.Line(), error.Column(), error.Hint());
    }
    if (bound_sql.size() > server.config.max_sql_bytes) {
        return fail(413, "SQL_TOO_LARGE", "SQL after parameter binding exceeds MAX_SQL_BYTES");
    }

    SqlStatement statement;
    try {
        statement = SqlParser(bound_sql).Parse();
    } catch (const SqlError& error) {
        bool legacy_integer_select = false;
        if (legacy) {
            try {
                const std::vector<SqlToken> tokens = SqlLexer::Tokenize(bound_sql);
                size_t content_end = tokens.size() - 1U;
                if (content_end != 0U && tokens[content_end - 1U].kind == SqlTokenKind::SEMICOLON) {
                    --content_end;
                }
                legacy_integer_select = content_end == 2U &&
                                        tokens[1].kind == SqlTokenKind::INTEGER &&
                                        EqualsIgnoreCase(tokens[0].text, "SELECT");
                if (!legacy_integer_select && content_end == 3U &&
                    tokens[1].kind == SqlTokenKind::INTEGER &&
                    tokens[2].kind == SqlTokenKind::INTEGER &&
                    EqualsIgnoreCase(tokens[0].text, "INSERT")) {
                    statement.kind = SqlStatementKind::INSERT;
                    legacy_integer_select = true;
                }
            } catch (const SqlError&) {
                legacy_integer_select = false;
            }
        }
        if (!legacy_integer_select) {
            return fail(HttpStatusFor(error), SqlErrorCodeName(error), error.what(), error.Line(),
                        error.Column(), error.Hint());
        }
        if (statement.kind != SqlStatementKind::INSERT) {
            statement.kind = SqlStatementKind::SELECT;
        }
    }

    if (server.draining.load(std::memory_order_acquire) ||
        server.recovering.load(std::memory_order_acquire)) {
        return fail(503, "UNAVAILABLE", "Service is recovering or draining");
    }
    const SqlStatementKind operation_kind =
        statement.kind == SqlStatementKind::EXPLAIN && statement.explained_statement
            ? statement.explained_statement->kind
            : statement.kind;
    if (IsWrite(operation_kind) && !server.config.api_key.empty() &&
        !ApiKeyMatches(server.config.api_key, request.get_header_value("X-API-Key"))) {
        return fail(401, "API_KEY_REQUIRED", "A valid X-API-Key is required");
    }
    if (server.config.read_only && operation_kind != SqlStatementKind::SELECT) {
        return fail(403, "READ_ONLY", "Only SELECT statements are allowed");
    }
    if (IsWrite(operation_kind) && server.config.max_db_bytes != 0U &&
        server.disk_manager->SizeInPages() >
            server.config.max_db_bytes / static_cast<uint64_t>(PAGE_SIZE)) {
        return fail(507, "STORAGE_QUOTA", "Database storage quota exceeded");
    }

    if (legacy) {
        try {
            crow::json::wvalue response;
            const auto started = std::chrono::steady_clock::now();
            const std::string result = server.executor->ExecuteQuery(bound_sql);
            const uint64_t elapsed = static_cast<uint64_t>(
                std::max<int64_t>(
                    0, std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count()));
            if (server.config.slow_query_ms != 0U &&
                elapsed >= server.config.slow_query_ms * 1000U) {
                crow::json::wvalue slow_log;
                slow_log["event"] = "slow_query";
                slow_log["request_id"] = request_id;
                slow_log["statement"] = SqlStatementName(statement.kind);
                slow_log["elapsed_us"] = elapsed;
                slow_log["sql"] = sql.substr(0, 200U);
                slow_log["plan"] = "(no execution plan for legacy statement)";
                std::clog << slow_log.dump() << '\n';
                StoreSlowQuery(server, request_id, SqlStatementName(statement.kind), elapsed, sql,
                               "(no execution plan for legacy statement)");
            }
            if (result.rfind("Syntax error:", 0) == 0 || result.rfind("Unknown command.", 0) == 0) {
                return fail(400, "SYNTAX_ERROR", result);
            }
            response["status"] = "success";
            response["result"] = result;
            response["deprecated"] = true;
            return JsonResponse(200, response);
        } catch (const DbError& error) {
            const int status = HttpStatusFor(error);
            return fail(status, ErrorCodeName(error),
                        status == 500 ? "Internal server error" : error.what());
        } catch (const std::exception&) {
            return fail(500, "INTERNAL", "Internal server error");
        }
    }

    const auto started = std::chrono::steady_clock::now();
    ActiveQueryRegistration active_query(
        server, request_id, SqlStatementName(statement.kind),
        std::chrono::milliseconds(server.config.statement_timeout_ms));
    if (active_query.Cancellation() == nullptr) {
        return fail(429, "BUSY", "Too many active statements or duplicate request ID");
    }
    try {
        PageTouchScope page_touch_scope;
        SqlExecutionResult result = server.executor->ExecuteResult(
            bound_sql, std::chrono::milliseconds(server.config.statement_timeout_ms),
            server.config.max_result_rows, active_query.Cancellation());
        const PageTouchCounts page_touches = page_touch_scope.Snapshot();
        const uint64_t elapsed = static_cast<uint64_t>(
            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count()));
        if (server.config.slow_query_ms != 0U &&
            elapsed >= server.config.slow_query_ms * 1000U) {
            crow::json::wvalue slow_log;
            slow_log["event"] = "slow_query";
            slow_log["request_id"] = request_id;
            slow_log["statement"] = SqlStatementName(statement.kind);
            slow_log["elapsed_us"] = elapsed;
            const std::string plan =
                result.plan.empty() ? "(no execution plan for this statement)" : result.plan;
            slow_log["sql"] = sql.substr(0, 200U);
            slow_log["plan"] = plan.substr(0, 200U);
            std::clog << slow_log.dump() << '\n';
            StoreSlowQuery(server, request_id, SqlStatementName(statement.kind), elapsed, sql,
                           plan);
        }
        const size_t result_limit = static_cast<size_t>(server.config.max_result_rows);
        const bool truncated = result.truncated || result.rows.size() > result_limit;
        if (truncated) {
            result.rows.resize(result_limit);
        }

        crow::json::wvalue response;
        response["ok"] = true;
        response["statement"] = SqlStatementName(statement.kind);
        crow::json::wvalue::list columns;
        for (size_t index = 0; index < result.column_names.size(); ++index) {
            crow::json::wvalue column;
            column["name"] = result.column_names[index];
            column["type"] = ColumnTypeName(result.columns[index].type);
            columns.push_back(std::move(column));
        }
        response["columns"] = std::move(columns);

        crow::json::wvalue::list rows;
        for (const Row& row : result.rows) {
            crow::json::wvalue::list values;
            for (const Value& value : row) {
                values.push_back(JsonValue(value));
            }
            rows.push_back(std::move(values));
        }
        response["rows"] = std::move(rows);
        response["row_count"] = static_cast<uint64_t>(result.rows.size());
        response["rows_affected"] = result.rows_affected;
        response["truncated"] = truncated;
        response["elapsed_us"] = elapsed;
        response["pages_touched"]["hit"] = page_touches.hit;
        response["pages_touched"]["miss"] = page_touches.miss;
        if (result.commit) {
            response["commit"]["lsn"] = result.commit->lsn;
            response["commit"]["pages_written"] = result.commit->pages_written;
            response["commit"]["bytes"] = result.commit->bytes;
            response["commit"]["fsync_us"] = result.commit->fsync_us;
        }
        if (statement.kind == SqlStatementKind::EXPLAIN) {
            response["plan"] = result.plan;
        }
        if (using_legacy_field) {
            response["deprecated"] = true;
        }
        return JsonResponse(200, response);
    } catch (const DbError& error) {
        const int status = HttpStatusFor(error);
        return fail(status, ErrorCodeName(error),
                    status == 500 ? "Internal server error" : error.what());
    } catch (const std::exception&) {
        return fail(500, "INTERNAL", "Internal server error");
    }
}

#if defined(__linux__) || defined(__APPLE__)
std::optional<uint64_t> ProcessRssBytes() {
#if defined(__linux__)
    std::ifstream status("/proc/self/statm");
    uint64_t virtual_pages = 0;
    uint64_t resident_pages = 0;
    if (!(status >> virtual_pages >> resident_pages)) {
        return std::nullopt;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    return page_size > 0
               ? std::optional<uint64_t>(resident_pages * static_cast<uint64_t>(page_size))
               : std::nullopt;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const kern_return_t result =
        task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count);
    return result == KERN_SUCCESS ? std::optional<uint64_t>(info.resident_size) : std::nullopt;
#endif
}

std::optional<uint64_t> ProcessOpenFileDescriptors() {
#if defined(__linux__) || defined(__APPLE__)
    struct DirectoryCloser {
        void operator()(DIR* directory) const noexcept {
            if (directory != nullptr) {
                closedir(directory);
            }
        }
    };
    std::unique_ptr<DIR, DirectoryCloser> directory(opendir("/dev/fd"));
    if (!directory) {
        return std::nullopt;
    }
    uint64_t count = 0;
    while (readdir(directory.get()) != nullptr) {
        ++count;
    }
    return count >= 3U ? std::optional<uint64_t>(count - 3U) : std::nullopt;
}
#endif
#endif

uint64_t SumStatementCounts(const MetricsRegistry& metrics) {
    uint64_t count = 0;
    for (const Counter& statement : metrics.statements) {
        count += statement.Value();
    }
    return count;
}

HistogramSnapshot CombinedStatementDuration(const MetricsRegistry& metrics) {
    HistogramSnapshot combined;
    for (const Histogram& histogram : metrics.statement_duration_us) {
        const HistogramSnapshot snapshot = histogram.Snapshot();
        combined.sum += snapshot.sum;
        combined.count += snapshot.count;
        for (size_t index = 0; index < combined.buckets.size(); ++index) {
            combined.buckets[index] += snapshot.buckets[index];
        }
    }
    return combined;
}

void AppendPrometheusHistogram(std::ostringstream& output, std::string_view name,
                               std::string_view help, const HistogramSnapshot& snapshot,
                               std::string_view label_name = {},
                               std::string_view label_value = {}, bool include_header = true) {
    if (include_header) {
        output << "# HELP " << name << ' ' << help << '\n'
               << "# TYPE " << name << " histogram\n";
    }
    uint64_t cumulative = 0;
    for (size_t index = 0; index + 1U < snapshot.buckets.size(); ++index) {
        cumulative += snapshot.buckets[index];
        const uint64_t upper_bound = uint64_t{1} << index;
        output << name << "_bucket{";
        if (!label_name.empty()) {
            output << label_name << "=\"" << label_value << "\",";
        }
        output << "le=\"" << upper_bound << "\"} " << cumulative << '\n';
    }
    output << name << "_bucket{";
    if (!label_name.empty()) {
        output << label_name << "=\"" << label_value << "\",";
    }
    output << "le=\"+Inf\"} " << snapshot.count << '\n'
           << name << "_sum";
    if (!label_name.empty()) {
        output << '{' << label_name << "=\"" << label_value << "\"}";
    }
    output << ' ' << snapshot.sum << '\n' << name << "_count";
    if (!label_name.empty()) {
        output << '{' << label_name << "=\"" << label_value << "\"}";
    }
    output << ' ' << snapshot.count << '\n';
}

void AddHistogramJson(crow::json::wvalue& target, const HistogramSnapshot& snapshot) {
    target["count"] = snapshot.count;
    target["sum_us"] = snapshot.sum;
    target["p50_us"] = snapshot.P50();
    target["p95_us"] = snapshot.P95();
    target["p99_us"] = snapshot.P99();
    crow::json::wvalue::list buckets;
    buckets.reserve(snapshot.buckets.size());
    for (uint64_t bucket : snapshot.buckets) {
        buckets.emplace_back(bucket);
    }
    target["buckets"] = std::move(buckets);
}

}  // namespace

void ApiServer::Impl::RegisterRoutes() {
    RegisterInspectorRoutes();

    CROW_ROUTE(app, "/<path>")
        .methods(crow::HTTPMethod::OPTIONS)([this](const crow::request& request,
                                                   crow::response& response,
                                                   const std::string& path) {
            (void)path;
            response = HandlePreflight(*this, request);
            response.end();
        });
    CROW_ROUTE(app, "/").methods(crow::HTTPMethod::OPTIONS)(
        [this](const crow::request& request) { return HandlePreflight(*this, request); });

    CROW_ROUTE(app, "/").methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
        return ServeStatic(*this, request, "index.html", "text/html; charset=utf-8");
    });
    CROW_ROUTE(app, "/index.css")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            return ServeStatic(*this, request, "index.css", "text/css; charset=utf-8");
        });
    CROW_ROUTE(app, "/index.js")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            return ServeStatic(*this, request, "index.js", "application/javascript; charset=utf-8");
        });
    CROW_ROUTE(app, "/fonts/IBMPlexMono-Regular.woff2")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            return ServeStatic(*this, request, "fonts/IBMPlexMono-Regular.woff2", "font/woff2");
        });
    CROW_ROUTE(app, "/js/<path>")
        .methods(crow::HTTPMethod::GET)(
            [this](const crow::request& request, const std::string& path) {
                return ServeStatic(*this, request, "js/" + path,
                                   "application/javascript; charset=utf-8");
            });

    CROW_ROUTE(app, "/healthz").methods(crow::HTTPMethod::GET)([]() {
        crow::json::wvalue response;
        response["ok"] = true;
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/readyz").methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
        auto& context = RequestContext(app, request);
        if (recovering.load(std::memory_order_acquire) ||
            draining.load(std::memory_order_acquire)) {
            return ErrorResponse(503, "NOT_READY", "Service is recovering or draining",
                                 context.request_id);
        }
        try {
            disk_manager->Sync();
        } catch (const DbError&) {
            return ErrorResponse(503, "NOT_READY", "Database is not writable", context.request_id);
        }
        crow::json::wvalue response;
        response["ok"] = true;
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/health").methods(crow::HTTPMethod::GET)([]() {
        crow::json::wvalue response;
        response["status"] = "ok";
        response["deprecated"] = true;
        return JsonResponse(200, response);
    });

    CROW_ROUTE(app, "/api/v1/query")
        .methods(crow::HTTPMethod::POST)(
            [this](const crow::request& request) { return HandleQuery(*this, request, false); });
    CROW_ROUTE(app, "/api/v1/query/<string>")
        .methods(crow::HTTPMethod::DELETE)(
            [this](const crow::request& request, const std::string& request_id) {
                ApiMiddleware::context& context = RequestContext(app, request);
                if (!IsSafeQueryRequestId(request_id)) {
                    return ErrorResponse(400, "INVALID_REQUEST_ID",
                                         "Request ID must be a bounded safe identifier",
                                         context.request_id);
                }
                if (!config.api_key.empty() &&
                    !ApiKeyMatches(config.api_key, request.get_header_value("X-API-Key"))) {
                    return ErrorResponse(401, "API_KEY_REQUIRED",
                                         "A valid X-API-Key is required", context.request_id);
                }
                std::shared_ptr<CancellationToken> cancellation;
                {
                    std::lock_guard<std::mutex> lock(active_queries_mutex);
                    const auto active = active_queries.find(request_id);
                    if (active != active_queries.end()) {
                        cancellation = active->second.cancellation;
                    }
                }
                if (!cancellation) {
                    return ErrorResponse(404, "QUERY_NOT_ACTIVE",
                                         "No active query has that request ID",
                                         context.request_id);
                }
                cancellation->Cancel();
                crow::json::wvalue response;
                response["ok"] = true;
                response["request_id"] = request_id;
                response["state"] = "cancellation_requested";
                return JsonResponse(202, response);
            });
    CROW_ROUTE(app, "/api/v1/batch")
        .methods(crow::HTTPMethod::POST)(
            [this](const crow::request& request) { return HandleBatch(*this, request); });
    CROW_ROUTE(app, "/api/v1/activity")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            ApiMiddleware::context& context = RequestContext(app, request);
            std::vector<ActiveQuery> queries;
            {
                std::lock_guard<std::mutex> lock(active_queries_mutex);
                queries.reserve(active_queries.size());
                for (const auto& entry : active_queries) {
                    queries.push_back(entry.second);
                }
            }
            std::sort(queries.begin(), queries.end(), [](const ActiveQuery& left,
                                                         const ActiveQuery& right) {
                return left.started < right.started;
            });
            crow::json::wvalue response;
            crow::json::wvalue::list activity;
            const auto now = std::chrono::steady_clock::now();
            for (const ActiveQuery& query : queries) {
                crow::json::wvalue item;
                item["request_id"] = query.request_id;
                item["state"] = "running";
                item["sql_preview"] = query.sql_preview;
                item["elapsed_ms"] = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - query.started)
                        .count());
                activity.push_back(std::move(item));
            }
            response["activity"] = std::move(activity);
            response["request_id"] = context.request_id;
            return JsonResponse(200, response);
        });
    CROW_ROUTE(app, "/api/query")
        .methods(crow::HTTPMethod::POST)(
            [this](const crow::request& request) { return HandleQuery(*this, request, true); });

    CROW_ROUTE(app, "/api/v1/tables").methods(crow::HTTPMethod::GET)([this]() {
        crow::json::wvalue response;
        crow::json::wvalue::list tables;
        for (const SqlTableMetadata& table : executor->GetTables()) {
            tables.emplace_back(table.name);
        }
        response["tables"] = std::move(tables);
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/api/v1/tables/<string>")
        .methods(crow::HTTPMethod::GET)(
            [this](const crow::request& request, const std::string& name) {
                auto& context = RequestContext(app, request);
                const std::optional<SqlTableMetadata> table = executor->GetTable(name);
                if (!table) {
                    return ErrorResponse(404, "UNKNOWN_TABLE", "Unknown table: " + name,
                                         context.request_id);
                }
                crow::json::wvalue response;
                response["name"] = table->name;
                response["row_estimate"] = table->row_estimate;
                crow::json::wvalue::list columns;
                for (const Column& column_info : table->schema.columns) {
                    crow::json::wvalue column;
                    column["name"] = column_info.name;
                    column["type"] = ColumnTypeName(column_info.type);
                    column["primary_key"] = table->schema.primary_key &&
                                            *table->schema.primary_key ==
                                                static_cast<size_t>(&column_info -
                                                                    table->schema.columns.data());
                    if (column_info.references) {
                        column["references"]["table"] = column_info.references->table;
                        column["references"]["column"] = column_info.references->column;
                    } else {
                        column["references"] = crow::json::wvalue(nullptr);
                    }
                    columns.push_back(std::move(column));
                }
                response["schema"] = std::move(columns);
                crow::json::wvalue::list indexes;
                for (const SqlIndexMetadata& index : table->indexes) {
                    crow::json::wvalue item;
                    item["name"] = index.name;
                    item["column"] = index.column;
                    item["unique"] = index.unique;
                    indexes.push_back(std::move(item));
                }
                response["indexes"] = std::move(indexes);
                return JsonResponse(200, response);
            });

    CROW_ROUTE(app, "/api/v1/catalog")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            const std::vector<SqlTableMetadata> tables =
                executor->GetTables(config.inspect_max_pages);
            crow::json::wvalue response;
            crow::json::wvalue::list table_list;
            table_list.reserve(tables.size());
            for (const SqlTableMetadata& table : tables) {
                crow::json::wvalue item;
                item["name"] = table.name;
                item["row_estimate"] = table.row_estimate;
                crow::json::wvalue::list columns;
                std::string ddl = "CREATE TABLE " + table.name + " (";
                for (size_t index = 0; index < table.schema.columns.size(); ++index) {
                    const Column& column = table.schema.columns[index];
                    if (index != 0U) {
                        ddl += ", ";
                    }
                    ddl += column.name + " ";
                    switch (column.type) {
                        case ColumnType::INT:
                            ddl += "INT";
                            break;
                        case ColumnType::BIGINT:
                            ddl += "BIGINT";
                            break;
                        case ColumnType::DOUBLE:
                            ddl += "DOUBLE";
                            break;
                        case ColumnType::BOOLEAN:
                            ddl += "BOOLEAN";
                            break;
                        case ColumnType::VARCHAR:
                            ddl += "VARCHAR(" + std::to_string(column.varchar_length) + ")";
                            break;
                    }
                    const bool primary_key =
                        table.schema.primary_key && *table.schema.primary_key == index;
                    if (primary_key) {
                        ddl += " PRIMARY KEY";
                    } else if (index < table.not_null.size() && table.not_null[index]) {
                        ddl += " NOT NULL";
                    }
                    if (column.references) {
                        ddl += " REFERENCES " + column.references->table + "(" +
                               column.references->column + ")";
                    }

                    crow::json::wvalue column_json;
                    column_json["name"] = column.name;
                    switch (column.type) {
                        case ColumnType::INT:
                            column_json["type"] = "INT";
                            break;
                        case ColumnType::BIGINT:
                            column_json["type"] = "BIGINT";
                            break;
                        case ColumnType::DOUBLE:
                            column_json["type"] = "DOUBLE";
                            break;
                        case ColumnType::BOOLEAN:
                            column_json["type"] = "BOOLEAN";
                            break;
                        case ColumnType::VARCHAR:
                            column_json["type"] = "VARCHAR";
                            break;
                    }
                    column_json["length"] = column.varchar_length;
                    column_json["nullable"] =
                        index >= table.not_null.size() || !table.not_null[index];
                    column_json["pk"] = primary_key;
                    if (column.references) {
                        column_json["references"]["table"] = column.references->table;
                        column_json["references"]["column"] = column.references->column;
                    } else {
                        column_json["references"] = crow::json::wvalue(nullptr);
                    }
                    columns.push_back(std::move(column_json));
                }
                ddl += ")";
                item["columns"] = std::move(columns);

                crow::json::wvalue::list indexes;
                for (const SqlIndexMetadata& index : table.indexes) {
                    crow::json::wvalue index_json;
                    index_json["name"] = index.name;
                    crow::json::wvalue::list index_columns;
                    index_columns.emplace_back(index.column);
                    index_json["columns"] = std::move(index_columns);
                    index_json["unique"] = index.unique;
                    index_json["kind"] = "btree";
                    index_json["meta_page"] = index.meta_page_id;
                    index_json["height"] = index.height;
                    index_json["pages"] = index.pages;
                    index_json["entries"] = index.entries;
                    index_json["leaf_pages"] = index.leaf_pages;
                    index_json["fill_factor"] = index.fill_factor;
                    indexes.push_back(std::move(index_json));
                    ddl += "; CREATE ";
                    if (index.unique) {
                        ddl += "UNIQUE ";
                    }
                    ddl += "INDEX " + index.name + " ON " + table.name + " (" + index.column + ")";
                }
                item["indexes"] = std::move(indexes);
                item["heap"]["first_page"] = table.heap.first_page;
                item["heap"]["pages"] = table.heap.pages;
                item["heap"]["tuples_estimate"] = table.heap.tuples;
                item["heap"]["bytes_used"] = table.heap.bytes_used;
                item["heap"]["bytes_free"] = table.heap.bytes_free;
                crow::json::wvalue::list free_space_histogram;
                constexpr std::array<std::string_view, 4> kFreeSpaceRanges{"0-25%", "25-50%",
                                                                           "50-75%", "75-100%"};
                for (size_t bucket = 0; bucket < kFreeSpaceRanges.size(); ++bucket) {
                    crow::json::wvalue item;
                    item["range"] = std::string(kFreeSpaceRanges[bucket]);
                    item["pages"] = table.heap.free_space_histogram[bucket];
                    free_space_histogram.push_back(std::move(item));
                }
                item["heap"]["free_space_histogram"] = std::move(free_space_histogram);
                item["ddl"] = RoundTripDdl(ddl);
                table_list.push_back(std::move(item));
            }
            response["tables"] = std::move(table_list);
            const std::string serialized = response.dump();
            if (serialized.size() > 8U * 1024U * 1024U) {
                ApiMiddleware::context& context = RequestContext(app, request);
                return ErrorResponse(413, "INSPECT_RESULT_TOO_LARGE",
                                     "Catalog response exceeds the 8 MiB inspection limit",
                                     context.request_id);
            }
            const std::string etag =
                "\"" + std::to_string(std::hash<std::string>{}(serialized)) + "\"";
            if (request.get_header_value("If-None-Match") == etag) {
                crow::response not_modified(304);
                not_modified.add_header("ETag", etag);
                not_modified.add_header("Cache-Control", "private, no-cache");
                return not_modified;
            }
            crow::response result = JsonResponse(200, response);
            result.add_header("ETag", etag);
            result.add_header("Cache-Control", "private, no-cache");
            return result;
        });

    CROW_ROUTE(app, "/api/v1/snapshot")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            const auto inspection_gate = executor->AcquireInspectionGate();
            (void)inspection_gate;
            const BufferPoolStats pool_stats = bpm->SnapshotStats();
            const std::vector<FrameInfo> frames = bpm->Snapshot();
            const DiskStats disk_stats = disk_manager->Stats();
            const MetricsRegistry& metrics = MetricsRegistry::Instance();
            const ApiMiddleware& middleware = app.get_middleware<ApiMiddleware>();
            const auto steady_now = std::chrono::steady_clock::now();
            const uint64_t page_count = disk_manager->SizeInPages();
            uint64_t dirty_count = 0;
            uint64_t pinned_count = 0;
            uint64_t resident_count = 0;
            std::vector<uint8_t> states;
            states.reserve(frames.size());
            for (const FrameInfo& frame : frames) {
                uint8_t state = frame.page_id == INVALID_PAGE_ID ? 0U : 1U;
                state |= frame.pin_count > 0 ? 1U << 1U : 0U;
                state |= frame.dirty ? 1U << 2U : 0U;
                state |= frame.ref_bit ? 1U << 3U : 0U;
                state |= static_cast<uint8_t>(frame.page_type) << 4U;
                states.push_back(state);
                resident_count += frame.page_id == INVALID_PAGE_ID ? 0U : 1U;
                dirty_count += frame.dirty ? 1U : 0U;
                pinned_count += frame.pin_count > 0 ? 1U : 0U;
            }

            double hit_ratio = 0.0;
            {
                std::lock_guard<std::mutex> lock(stats_window_mutex);
                if (previous_stats_at.time_since_epoch().count() != 0) {
                    const uint64_t hits = pool_stats.hits >= previous_stats_hits
                                              ? pool_stats.hits - previous_stats_hits
                                              : 0U;
                    const uint64_t misses = pool_stats.misses >= previous_stats_misses
                                                ? pool_stats.misses - previous_stats_misses
                                                : 0U;
                    const uint64_t lookups = hits + misses;
                    hit_ratio = lookups == 0U ? 0.0 : static_cast<double>(hits) / lookups;
                }
                previous_stats_hits = pool_stats.hits;
                previous_stats_misses = pool_stats.misses;
                previous_stats_at = steady_now;
            }

            crow::json::wvalue response;
            response["ts_ms"] = (UnixTimeMs() / 1000U) * 1000U;
            response["engine"]["version"] = CUSTOMRELATIONALDB_VERSION;
            response["engine"]["git_sha"] = RELDB_GIT_SHA;
            response["engine"]["format_version"] = 2;
            response["engine"]["read_only"] = config.read_only;
            response["engine"]["durability"] = config.durability;
            response["engine"]["pool_policy"] = config.buffer_pool_policy;
            response["engine"]["pool_frames"] = bpm->PoolSize();
            response["engine"]["uptime_s"] =
                std::chrono::duration_cast<std::chrono::seconds>(steady_now - middleware.started_at)
                    .count();
            response["engine"]["demo_mode"] = config.demo_load;
            response["engine"]["next_reset_at"] = crow::json::wvalue(nullptr);

            response["stats"]["queries"] = middleware.queries.load(std::memory_order_relaxed);
            response["stats"]["rows_read"] = metrics.rows_read.Value();
            response["stats"]["rows_written"] = metrics.rows_written.Value();
            response["stats"]["errors"] = metrics.ErrorCount();
            response["stats"]["index_scans"] = metrics.index_scans.Value();
            response["stats"]["sequential_scans"] = metrics.sequential_scans.Value();

            response["storage"]["file_size_bytes"] = disk_manager->FileSizeBytes();
            response["storage"]["page_count"] = page_count;
            response["storage"]["free_pages"] = bpm->FreePageCount();
            response["storage"]["disk_reads"] = disk_stats.reads;
            response["storage"]["disk_writes"] = disk_stats.writes;

            response["pool"]["size"] = bpm->PoolSize();
            response["pool"]["pinned"] = pinned_count;
            response["pool"]["dirty"] = dirty_count;
            response["pool"]["free"] = frames.size() - resident_count;
            response["pool"]["hit_ratio_window"] = hit_ratio;

            if (WriteAheadLog* wal = executor->Wal()) {
                const WalRecoveryStats recovery = wal->LastRecoveryStats();
                response["wal"]["size_bytes"] = wal->SizeBytes();
                response["wal"]["last_lsn"] = wal->LastLSN();
                response["wal"]["checkpoints"] = wal->CheckpointCount();
                response["wal"]["last_checkpoint_unix_ms"] = wal->LastCheckpointUnixMs();
                response["wal"]["recovery"]["ran"] = recovery.ran;
                response["wal"]["recovery"]["records"] = recovery.records;
                response["wal"]["recovery"]["commits"] = recovery.commits;
                response["wal"]["recovery"]["pages_replayed"] = recovery.pages_replayed;
            } else {
                response["wal"]["size_bytes"] = 0;
                response["wal"]["last_lsn"] = 0;
                response["wal"]["checkpoints"] = 0;
                response["wal"]["last_checkpoint_unix_ms"] = 0;
                response["wal"]["recovery"]["ran"] = false;
            }
            response["activity"]["active"] = metrics.active_statements.Value();
            response["activity"]["waiting"] = metrics.statement_waiters.Value();

            std::string serialized = response.dump();
            const std::string etag =
                "\"" + std::to_string(std::hash<std::string>{}(serialized)) + "\"";
            if (request.get_header_value("If-None-Match") == etag) {
                crow::response not_modified(304);
                not_modified.add_header("ETag", etag);
                return not_modified;
            }
            crow::response result(200, std::move(serialized));
            result.add_header("Content-Type", "application/json; charset=utf-8");
            result.add_header("ETag", etag);
            return result;
        });

    CROW_ROUTE(app, "/api/v1/stats").methods(crow::HTTPMethod::GET)([this]() {
        const BufferPoolStats pool = bpm->SnapshotStats();
        const DiskStats disk = disk_manager->Stats();
        MetricsRegistry& metrics = MetricsRegistry::Instance();
        const auto now = std::chrono::steady_clock::now();
        double hit_rate_window = 0.0;
        double window_seconds = 0.0;
        {
            std::lock_guard<std::mutex> lock(stats_window_mutex);
            if (previous_stats_at.time_since_epoch().count() != 0) {
                const uint64_t hit_delta =
                    pool.hits >= previous_stats_hits ? pool.hits - previous_stats_hits : 0U;
                const uint64_t miss_delta = pool.misses >= previous_stats_misses
                                                ? pool.misses - previous_stats_misses
                                                : 0U;
                const uint64_t lookups = hit_delta + miss_delta;
                hit_rate_window =
                    lookups == 0U ? 0.0 : static_cast<double>(hit_delta) / lookups;
                window_seconds =
                    std::chrono::duration<double>(now - previous_stats_at).count();
            }
            previous_stats_hits = pool.hits;
            previous_stats_misses = pool.misses;
            previous_stats_at = now;
        }

        crow::json::wvalue response;
        response["uptime_seconds"] =
            std::chrono::duration<double>(now - app.get_middleware<ApiMiddleware>().started_at)
                .count();
        response["demo_load"] = metrics.demo_load_active.Value() != 0;
        response["buffer_pool"]["size"] = bpm->PoolSize();
        response["buffer_pool"]["hits"] = pool.hits;
        response["buffer_pool"]["misses"] = pool.misses;
        response["buffer_pool"]["evictions"] = pool.evictions;
        response["buffer_pool"]["dirty_write_backs"] = pool.dirty_write_backs;
        response["buffer_pool"]["waits"] = pool.waits;
        response["buffer_pool"]["pinned_frames"] = bpm->pinned_frames();
        response["buffer_pool"]["hit_rate_window"] = hit_rate_window;
        response["buffer_pool"]["hit_rate_window_seconds"] = window_seconds;
        response["disk"]["reads"] = disk.reads;
        response["disk"]["writes"] = disk.writes;
        response["disk"]["syncs"] = disk.syncs;
        response["disk"]["bytes"] = disk.bytes;
        AddHistogramJson(response["disk"]["read_latency_us"], metrics.disk_read_us.Snapshot());
        AddHistogramJson(response["disk"]["write_latency_us"], metrics.disk_write_us.Snapshot());
        AddHistogramJson(response["disk"]["sync_latency_us"], metrics.disk_sync_us.Snapshot());
        response["requests"] =
            app.get_middleware<ApiMiddleware>().requests.load(std::memory_order_relaxed);
        response["queries"] =
            app.get_middleware<ApiMiddleware>().queries.load(std::memory_order_relaxed);
        response["errors"] = metrics.ErrorCount();
        response["rows_read"] = metrics.rows_read.Value();
        response["rows_written"] = metrics.rows_written.Value();
        response["index_scans"] = metrics.index_scans.Value();
        response["sequential_scans"] = metrics.sequential_scans.Value();
        response["active_statements"] = metrics.active_statements.Value();
        response["statement_waiters"] = metrics.statement_waiters.Value();
        response["rate_limited_requests"] = metrics.rate_limited_requests.Value();
        response["inspect_rate_limited_requests"] = metrics.inspect_rate_limited_requests.Value();
        response["busy_rejections"] = metrics.busy_rejections.Value();
        crow::json::wvalue statements;
        crow::json::wvalue statement_durations;
        for (size_t index = 0; index < MetricsRegistry::kStatementLabels.size(); ++index) {
            const std::string label(MetricsRegistry::kStatementLabels[index]);
            statements[label] = metrics.statements[index].Value();
            AddHistogramJson(statement_durations[label],
                             metrics.statement_duration_us[index].Snapshot());
        }
        response["statements"] = std::move(statements);
        response["statement_duration_us"] = std::move(statement_durations);
        crow::json::wvalue http_requests;
        for (size_t route = 0; route < MetricsRegistry::kHttpRouteLabels.size(); ++route) {
            crow::json::wvalue statuses;
            for (size_t status = 0; status < MetricsRegistry::kHttpStatusLabels.size(); ++status) {
                statuses[std::string(MetricsRegistry::kHttpStatusLabels[status])] =
                    metrics.http_requests[route][status].Value();
            }
            http_requests[std::string(MetricsRegistry::kHttpRouteLabels[route])] =
                std::move(statuses);
        }
        response["http_requests"] = std::move(http_requests);
        crow::json::wvalue errors;
        for (size_t index = 0; index < MetricsRegistry::kErrorLabels.size(); ++index) {
            errors[std::string(MetricsRegistry::kErrorLabels[index])] =
                metrics.errors[index].Value();
        }
        response["errors_by_code"] = std::move(errors);
        response["wal"]["bytes"] = metrics.wal_bytes.Value();
        response["wal"]["commits"] = metrics.wal_commits.Value();
        response["wal"]["sync_latency_us"]["count"] = metrics.wal_sync_us.Snapshot().count;
        response["wal"]["sync_latency_us"]["sum_us"] = metrics.wal_sync_us.Snapshot().sum;
        response["wal"]["checkpoints"] = metrics.checkpoints.Value();
        AddHistogramJson(response["wal"]["checkpoint_duration_us"],
                         metrics.checkpoint_duration_us.Snapshot());
        AddHistogramJson(response["wal"]["recovery_duration_us"],
                         metrics.recovery_duration_us.Snapshot());
        response["wal"]["recovery_records"] = metrics.recovery_records.Value();
        response["wal"]["recovery_ran"] = metrics.recovery_ran.Value() != 0;
        if (WriteAheadLog* wal = executor->Wal()) {
            response["wal"]["size_bytes"] = wal->SizeBytes();
            response["wal"]["commit_count"] = wal->CommitCount();
            response["wal"]["checkpoint_count"] = wal->CheckpointCount();
            const uint64_t checkpoint_ms = wal->LastCheckpointUnixMs();
            response["wal"]["last_checkpoint_unix_ms"] =
                checkpoint_ms == 0U ? crow::json::wvalue(nullptr)
                                    : crow::json::wvalue(checkpoint_ms);
        }
        const HistogramSnapshot request_latency = metrics.http_request_duration_us.Snapshot();
        AddHistogramJson(response["http_request_duration_us"], request_latency);
#if defined(__linux__) || defined(__APPLE__)
        if (const auto rss_bytes = ProcessRssBytes()) {
            response["process"]["rss_bytes"] = *rss_bytes;
        }
        if (const auto open_fds = ProcessOpenFileDescriptors()) {
            response["process"]["open_fds"] = *open_fds;
        }
#endif
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/api/v1/stats/buffer_pool")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& request) {
            const char* format = request.url_params.get("format");
            if (format != nullptr) {
                const std::string_view format_value(format);
                if (format_value.size() > 7U ||
                    (format_value != "compact" && format_value != "legacy")) {
                    return ErrorResponse(400, "INVALID_ARGUMENT",
                                         "format must be compact or legacy",
                                         RequestContext(app, request).request_id);
                }
            }
            const auto inspection_gate = executor->AcquireInspectionGate();
            (void)inspection_gate;
            const std::vector<FrameInfo> frames = bpm->Snapshot();
            crow::json::wvalue response;
            response["pool_size"] = bpm->PoolSize();
            response["snapshot_size"] = frames.size();
            response["pinned_frames"] = bpm->pinned_frames();
            if (format != nullptr && std::string_view(format) == "legacy") {
                crow::json::wvalue::list frame_list;
                frame_list.reserve(frames.size());
                for (const FrameInfo& frame : frames) {
                    crow::json::wvalue item;
                    item["frame_id"] = frame.frame_id;
                    item["page_id"] = frame.page_id;
                    item["pin_count"] = frame.pin_count;
                    item["dirty"] = frame.dirty;
                    item["ref_bit"] = frame.ref_bit;
                    item["clock_hand"] = frame.clock_hand;
                    frame_list.push_back(std::move(item));
                }
                response["frames"] = std::move(frame_list);
            } else {
                std::vector<uint8_t> states;
                states.reserve(frames.size());
                for (const FrameInfo& frame : frames) {
                    uint8_t state = frame.page_id == INVALID_PAGE_ID ? 0U : 1U;
                    state |= frame.pin_count > 0 ? 1U << 1U : 0U;
                    state |= frame.dirty ? 1U << 2U : 0U;
                    state |= frame.ref_bit ? 1U << 3U : 0U;
                    state |= static_cast<uint8_t>(frame.page_type) << 4U;
                    states.push_back(state);
                }
                response["encoding"] = "base64-state-v1";
                response["states_base64"] = Base64Encode(states);
            }
            return JsonResponse(200, response);
        });
    CROW_ROUTE(app, "/api/v1/stats/storage").methods(crow::HTTPMethod::GET)([this]() {
        crow::json::wvalue response;
        response["file_size_bytes"] = disk_manager->FileSizeBytes();
        response["page_count"] = disk_manager->SizeInPages();
        response["free_pages"] = bpm->FreePageCount();
        WriteAheadLog* wal = executor->Wal();
        if (wal != nullptr) {
            response["wal_size_bytes"] = wal->SizeBytes();
            response["checkpoint_count"] = wal->CheckpointCount();
            const uint64_t last_checkpoint = wal->LastCheckpointUnixMs();
            response["last_checkpoint_unix_ms"] =
                last_checkpoint == 0U ? crow::json::wvalue(nullptr)
                                     : crow::json::wvalue(last_checkpoint);
            const WalRecoveryStats recovery = wal->LastRecoveryStats();
            response["recovery"]["ran"] = recovery.ran;
            response["recovery"]["records"] = recovery.records;
            response["recovery"]["commits"] = recovery.commits;
            response["recovery"]["pages_replayed"] = recovery.pages_replayed;
            response["recovery"]["truncated_bytes"] = recovery.truncated_bytes;
            response["recovery"]["duration_us"] = recovery.elapsed_us;
        } else {
            response["wal_size_bytes"] = 0;
            response["checkpoint_count"] = 0;
            response["last_checkpoint_unix_ms"] = crow::json::wvalue(nullptr);
            response["recovery"]["ran"] = false;
            response["recovery"]["records"] = 0;
            response["recovery"]["commits"] = 0;
            response["recovery"]["pages_replayed"] = 0;
            response["recovery"]["truncated_bytes"] = 0;
            response["recovery"]["duration_us"] = 0;
        }
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/api/metrics").methods(crow::HTTPMethod::GET)([this]() {
        const BufferPoolStats stats = bpm->SnapshotStats();
        const MetricsRegistry& metrics = MetricsRegistry::Instance();
        const uint64_t queries = SumStatementCounts(metrics);
        const HistogramSnapshot latency = CombinedStatementDuration(metrics);
        HistogramSnapshot io_latency = metrics.disk_read_us.Snapshot();
        const HistogramSnapshot write_latency = metrics.disk_write_us.Snapshot();
        const HistogramSnapshot sync_latency = metrics.disk_sync_us.Snapshot();
        io_latency.sum += write_latency.sum + sync_latency.sum;
        io_latency.count += write_latency.count + sync_latency.count;
        const uint64_t lookups = stats.hits + stats.misses;
        crow::json::wvalue response;
        response["cache_hit_rate"] =
            lookups == 0U ? 0.0 : static_cast<double>(stats.hits) / lookups;
        const double uptime_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          app.get_middleware<ApiMiddleware>().started_at)
                .count();
        response["query_throughput"] =
            uptime_seconds > 0.0 ? static_cast<double>(queries) / uptime_seconds : 0.0;
        response["avg_latency"] =
            latency.count == 0U ? 0.0
                                : static_cast<double>(latency.sum) / latency.count / 1000.0;
        response["avg_io_latency"] =
            io_latency.count == 0U
                ? 0.0
                : static_cast<double>(io_latency.sum) / io_latency.count / 1000.0;
        response["active_latches"] = metrics.active_statements.Value();
        response["active_statements"] = metrics.active_statements.Value();
        response["error_count"] = metrics.ErrorCount();
        response["source"] = "live";
        response["deprecated"] = true;
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/metrics").methods(crow::HTTPMethod::GET)([this]() {
        const BufferPoolStats pool = bpm->SnapshotStats();
        const DiskStats disk = disk_manager->Stats();
        const ApiMiddleware& middleware = app.get_middleware<ApiMiddleware>();
        const MetricsRegistry& metrics = MetricsRegistry::Instance();
        std::ostringstream text;
        text << std::setprecision(17);
        text << "# HELP reldb_http_requests_total HTTP requests by route and status.\n"
             << "# TYPE reldb_http_requests_total counter\n";
        for (size_t route = 0; route < MetricsRegistry::kHttpRouteLabels.size(); ++route) {
            for (size_t status = 0; status < MetricsRegistry::kHttpStatusLabels.size(); ++status) {
                text << "reldb_http_requests_total{route=\""
                     << MetricsRegistry::kHttpRouteLabels[route] << "\",status=\""
                     << MetricsRegistry::kHttpStatusLabels[status] << "\"} "
                     << metrics.http_requests[route][status].Value() << '\n';
            }
        }
        text << "# HELP reldb_statements_total SQL statements by type.\n"
             << "# TYPE reldb_statements_total counter\n";
        for (size_t index = 0; index < MetricsRegistry::kStatementLabels.size(); ++index) {
            text << "reldb_statements_total{type=\"" << MetricsRegistry::kStatementLabels[index]
                 << "\"} " << metrics.statements[index].Value() << '\n';
            AppendPrometheusHistogram(
                text, "reldb_statement_duration_us",
                "SQL statement duration in microseconds.",
                metrics.statement_duration_us[index].Snapshot(), "type",
                MetricsRegistry::kStatementLabels[index], index == 0U);
        }
        text << "# HELP reldb_rows_read_total Rows fetched by table scans and point lookups.\n"
             << "# TYPE reldb_rows_read_total counter\n"
             << "reldb_rows_read_total " << metrics.rows_read.Value() << '\n'
             << "# HELP reldb_rows_written_total Rows affected by write statements.\n"
             << "# TYPE reldb_rows_written_total counter\n"
             << "reldb_rows_written_total " << metrics.rows_written.Value() << '\n'
             << "# HELP reldb_plan_scans_total Query plan scans by type.\n"
             << "# TYPE reldb_plan_scans_total counter\n"
             << "reldb_plan_scans_total{type=\"index\"} " << metrics.index_scans.Value() << '\n'
             << "reldb_plan_scans_total{type=\"seq\"} " << metrics.sequential_scans.Value() << '\n'
             << "# HELP reldb_errors_total SQL and API failures by error code.\n"
             << "# TYPE reldb_errors_total counter\n";
        for (size_t index = 0; index < MetricsRegistry::kErrorLabels.size(); ++index) {
            text << "reldb_errors_total{code=\"" << MetricsRegistry::kErrorLabels[index] << "\"} "
                 << metrics.errors[index].Value() << '\n';
        }
        text << "# HELP reldb_rejected_requests_total Requests rejected by admission controls.\n"
             << "# TYPE reldb_rejected_requests_total counter\n"
             << "reldb_rejected_requests_total{reason=\"rate_limit\"} "
             << metrics.rate_limited_requests.Value() << '\n'
             << "reldb_rejected_requests_total{reason=\"busy\"} " << metrics.busy_rejections.Value()
             << '\n'
             << "reldb_rejected_requests_total{reason=\"inspect_rate_limit\"} "
             << metrics.inspect_rate_limited_requests.Value() << '\n'
             << "# HELP reldb_buffer_pool_hits_total Buffer pool cache hits.\n"
             << "# TYPE reldb_buffer_pool_hits_total counter\n"
             << "reldb_buffer_pool_hits_total " << pool.hits << '\n'
             << "# HELP reldb_buffer_pool_misses_total Buffer pool cache misses.\n"
             << "# TYPE reldb_buffer_pool_misses_total counter\n"
             << "reldb_buffer_pool_misses_total " << pool.misses << '\n'
             << "# HELP reldb_buffer_pool_evictions_total Buffer pool evictions.\n"
             << "# TYPE reldb_buffer_pool_evictions_total counter\n"
             << "reldb_buffer_pool_evictions_total " << pool.evictions << '\n'
             << "# HELP reldb_buffer_pool_dirty_write_backs_total Dirty pages written back.\n"
             << "# TYPE reldb_buffer_pool_dirty_write_backs_total counter\n"
             << "reldb_buffer_pool_dirty_write_backs_total " << pool.dirty_write_backs << '\n'
             << "# HELP reldb_buffer_pool_waits_total Buffer pool waits.\n"
             << "# TYPE reldb_buffer_pool_waits_total counter\n"
             << "reldb_buffer_pool_waits_total " << pool.waits << '\n'
             << "# HELP reldb_buffer_pool_pinned_frames Current pinned frames.\n"
             << "# TYPE reldb_buffer_pool_pinned_frames gauge\n"
             << "reldb_buffer_pool_pinned_frames " << bpm->pinned_frames() << '\n'
             << "# HELP reldb_disk_reads_total Database page reads.\n"
             << "# TYPE reldb_disk_reads_total counter\n"
             << "reldb_disk_reads_total " << disk.reads << '\n'
             << "# HELP reldb_disk_writes_total Database page writes.\n"
             << "# TYPE reldb_disk_writes_total counter\n"
             << "reldb_disk_writes_total " << disk.writes << '\n'
             << "# HELP reldb_disk_syncs_total Database sync operations.\n"
             << "# TYPE reldb_disk_syncs_total counter\n"
             << "reldb_disk_syncs_total " << disk.syncs << '\n'
             << "# HELP reldb_disk_bytes_total Database I/O bytes.\n"
             << "# TYPE reldb_disk_bytes_total counter\n"
             << "reldb_disk_bytes_total " << disk.bytes << '\n';
        AppendPrometheusHistogram(text, "reldb_disk_read_duration_us",
                                  "Database read latency in microseconds.",
                                  metrics.disk_read_us.Snapshot());
        AppendPrometheusHistogram(text, "reldb_disk_write_duration_us",
                                  "Database write latency in microseconds.",
                                  metrics.disk_write_us.Snapshot());
        AppendPrometheusHistogram(text, "reldb_disk_sync_duration_us",
                                  "Database sync latency in microseconds.",
                                  metrics.disk_sync_us.Snapshot());
        AppendPrometheusHistogram(text, "reldb_http_request_duration_us",
                                  "HTTP request duration in microseconds.",
                                  metrics.http_request_duration_us.Snapshot());
        AppendPrometheusHistogram(text, "reldb_statement_gate_wait_duration_us",
                                  "Statement gate wait duration in microseconds.",
                                  metrics.gate_wait_us.Snapshot());
        text << "# HELP reldb_statement_gate_active Current active statements.\n"
             << "# TYPE reldb_statement_gate_active gauge\n"
             << "reldb_statement_gate_active " << metrics.active_statements.Value() << '\n'
             << "# HELP reldb_statement_gate_waiters Current queued statements.\n"
             << "# TYPE reldb_statement_gate_waiters gauge\n"
             << "reldb_statement_gate_waiters " << metrics.statement_waiters.Value() << '\n'
             << "# HELP reldb_wal_bytes_total WAL bytes appended.\n"
             << "# TYPE reldb_wal_bytes_total counter\n"
             << "reldb_wal_bytes_total " << metrics.wal_bytes.Value() << '\n'
             << "# HELP reldb_wal_commits_total WAL commits.\n"
             << "# TYPE reldb_wal_commits_total counter\n"
             << "reldb_wal_commits_total " << metrics.wal_commits.Value() << '\n';
        AppendPrometheusHistogram(text, "reldb_wal_sync_duration_us",
                                  "WAL fsync latency in microseconds.",
                                  metrics.wal_sync_us.Snapshot());
        text << "# HELP reldb_checkpoints_total Completed checkpoints.\n"
             << "# TYPE reldb_checkpoints_total counter\n"
             << "reldb_checkpoints_total " << metrics.checkpoints.Value() << '\n';
        if (WriteAheadLog* wal = executor->Wal()) {
            text << "# HELP reldb_wal_size_bytes Current WAL file size in bytes.\n"
                 << "# TYPE reldb_wal_size_bytes gauge\n"
                 << "reldb_wal_size_bytes " << wal->SizeBytes() << '\n'
                 << "# HELP reldb_last_checkpoint_unix_ms Last completed checkpoint timestamp.\n"
                 << "# TYPE reldb_last_checkpoint_unix_ms gauge\n"
                 << "reldb_last_checkpoint_unix_ms " << wal->LastCheckpointUnixMs() << '\n';
        }
        AppendPrometheusHistogram(text, "reldb_checkpoint_duration_us",
                                  "Checkpoint duration in microseconds.",
                                  metrics.checkpoint_duration_us.Snapshot());
        text << "# HELP reldb_recovery_records_total WAL records processed during recovery.\n"
             << "# TYPE reldb_recovery_records_total counter\n"
             << "reldb_recovery_records_total " << metrics.recovery_records.Value() << '\n';
        AppendPrometheusHistogram(text, "reldb_recovery_duration_us",
                                  "Recovery duration in microseconds.",
                                  metrics.recovery_duration_us.Snapshot());
        text << "# HELP reldb_uptime_seconds Process uptime in seconds.\n"
             << "# TYPE reldb_uptime_seconds gauge\n"
             << "reldb_uptime_seconds "
             << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              middleware.started_at)
                    .count()
             << '\n'
             << "# HELP reldb_recovery_ran Whether WAL recovery completed during this process.\n"
             << "# TYPE reldb_recovery_ran gauge\n"
             << "reldb_recovery_ran " << (metrics.recovery_ran.Value() != 0 ? 1 : 0) << '\n'
             << "# HELP reldb_demo_load_active Whether the optional demo workload is active.\n"
             << "# TYPE reldb_demo_load_active gauge\n"
             << "reldb_demo_load_active " << (metrics.demo_load_active.Value() != 0 ? 1 : 0)
             << '\n';
#if defined(__linux__) || defined(__APPLE__)
        if (const auto rss_bytes = ProcessRssBytes()) {
            text << "# HELP reldb_process_rss_bytes Resident set size in bytes.\n"
                 << "# TYPE reldb_process_rss_bytes gauge\n"
                 << "reldb_process_rss_bytes " << *rss_bytes << '\n';
        }
        if (const auto open_fds = ProcessOpenFileDescriptors()) {
            text << "# HELP reldb_process_open_fds Current open file descriptors.\n"
                 << "# TYPE reldb_process_open_fds gauge\n"
                 << "reldb_process_open_fds " << *open_fds << '\n';
        }
#endif
        crow::response response(text.str());
        response.add_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        return response;
    });
    CROW_ROUTE(app, "/api/v1/version").methods(crow::HTTPMethod::GET)([]() {
        crow::json::wvalue response;
        response["git_sha"] = RELDB_GIT_SHA;
        response["build_time"] = RELDB_BUILD_TIME;
        response["format_version"] = 2;
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/api/v1/admin/checkpoint")
        .methods(crow::HTTPMethod::POST)([this](const crow::request& request) {
            auto& context = RequestContext(app, request);
            if (config.api_key.empty() ||
                !ApiKeyMatches(config.api_key, request.get_header_value("X-API-Key"))) {
                return ErrorResponse(401, "API_KEY_REQUIRED", "A valid X-API-Key is required",
                                     context.request_id);
            }
            try {
                const std::string result = executor->ExecuteQuery("CHECKPOINT");
                crow::json::wvalue response;
                response["ok"] = true;
                response["result"] = result;
                return JsonResponse(200, response);
            } catch (const DbError& error) {
                const int status = HttpStatusFor(error);
                return ErrorResponse(status, ErrorCodeName(error),
                                     status == 500 ? "Internal server error" : error.what(),
                                     context.request_id);
            } catch (const std::exception&) {
                return ErrorResponse(500, "INTERNAL", "Internal server error", context.request_id);
            }
        });

    CROW_ROUTE(app, "/api/buffer_pool").methods(crow::HTTPMethod::GET)([this]() {
        const BufferPoolStats stats = bpm->SnapshotStats();
        crow::json::wvalue response;
        response["pinned_frames"] = bpm->pinned_frames();
        response["hits"] = stats.hits;
        response["misses"] = stats.misses;
        response["evictions"] = stats.evictions;
        response["dirty_write_backs"] = stats.dirty_write_backs;
        response["waits"] = stats.waits;
        crow::json::wvalue::list frames;
        for (const FrameInfo& info : bpm->Snapshot()) {
            crow::json::wvalue frame;
            frame["frame_id"] = info.frame_id;
            frame["page_id"] = info.page_id;
            frame["pin_count"] = info.pin_count;
            frame["dirty"] = info.dirty;
            frame["ref_bit"] = info.ref_bit;
            frame["clock_hand"] = info.clock_hand;
            frames.push_back(std::move(frame));
        }
        response["frames"] = std::move(frames);
        return JsonResponse(200, response);
    });

    CROW_ROUTE(app, "/api/v1/query")
        .methods(crow::HTTPMethod::OPTIONS)(
            [this](const crow::request& request) { return HandlePreflight(*this, request); });
    CROW_ROUTE(app, "/api/query")
        .methods(crow::HTTPMethod::OPTIONS)(
            [this](const crow::request& request) { return HandlePreflight(*this, request); });
    CROW_ROUTE(app, "/api/v1/admin/checkpoint")
        .methods(crow::HTTPMethod::OPTIONS)(
            [this](const crow::request& request) { return HandlePreflight(*this, request); });

    CROW_ROUTE(app, "/api/v1/<path>")
        .methods(crow::HTTPMethod::GET)(
            [this](const crow::request& request, const std::string& path) {
                (void)path;
                ApiMiddleware::context& context = RequestContext(app, request);
                return ErrorResponse(404, "NOT_FOUND", "API route not found", context.request_id);
            });
    CROW_ROUTE(app, "/api/v1/<path>")
        .methods(crow::HTTPMethod::POST, crow::HTTPMethod::PUT, crow::HTTPMethod::PATCH,
                 crow::HTTPMethod::DELETE, crow::HTTPMethod::HEAD)(
            [this](const crow::request& request, const std::string& path) {
                (void)path;
                ApiMiddleware::context& context = RequestContext(app, request);
                return ErrorResponse(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                                     context.request_id);
            });

    CROW_ROUTE(app, "/<path>")
        .methods(crow::HTTPMethod::GET)(
            [this](const crow::request& request, const std::string& path) {
                (void)path;
                ApiMiddleware::context& context = RequestContext(app, request);
                return ErrorResponse(404, "NOT_FOUND", "Route not found", context.request_id);
            });
    CROW_ROUTE(app, "/<path>")
        .methods(crow::HTTPMethod::POST, crow::HTTPMethod::PUT, crow::HTTPMethod::PATCH,
                 crow::HTTPMethod::DELETE, crow::HTTPMethod::HEAD)(
            [this](const crow::request& request, const std::string& path) {
                (void)path;
                ApiMiddleware::context& context = RequestContext(app, request);
                return ErrorResponse(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                                     context.request_id);
            });
}

}  // namespace relational_db
