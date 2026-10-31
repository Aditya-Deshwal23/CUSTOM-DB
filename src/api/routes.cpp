#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <utility>

#include "internal.h"

namespace relational_db {
namespace {

struct QueryTiming {
    explicit QueryTiming(ApiMiddleware& api_middleware)
        : middleware(api_middleware), start(std::chrono::steady_clock::now()) {}
    ~QueryTiming() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        middleware.query_latency_us.fetch_add(static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)),
                                              std::memory_order_relaxed);
    }

    ApiMiddleware& middleware;
    std::chrono::steady_clock::time_point start;
};

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

ApiMiddleware::context& RequestContext(crow::App<ApiMiddleware>& app,
                                       const crow::request& request) {
    return app.get_context<ApiMiddleware>(request);
}

bool IsWrite(SqlStatementKind kind) {
    return kind != SqlStatementKind::SELECT && kind != SqlStatementKind::EXPLAIN &&
           kind != SqlStatementKind::SHOW_TABLES && kind != SqlStatementKind::DESCRIBE;
}

std::string ErrorCodeName(const DbError& error) {
    switch (error.Code()) {
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
        case ErrorCode::BUFFER_POOL_EXHAUSTED:
            return "BUSY";
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
    QueryTiming query_timing(api_middleware);
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
            const std::string result = server.executor->ExecuteQuery(bound_sql);
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
    try {
        SqlExecutionResult result = server.executor->ExecuteResult(
            bound_sql, std::chrono::milliseconds(server.config.statement_timeout_ms),
            server.config.max_result_rows);
        const uint64_t elapsed = static_cast<uint64_t>(
            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count()));
        const size_t result_limit = static_cast<size_t>(server.config.max_result_rows);
        const bool truncated = result.rows.size() > result_limit;
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

}  // namespace

void ApiServer::Impl::RegisterRoutes() {
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

    CROW_ROUTE(app, "/api/v1/stats").methods(crow::HTTPMethod::GET)([this]() {
        const BufferPoolStats pool = bpm->SnapshotStats();
        const DiskStats disk = disk_manager->Stats();
        crow::json::wvalue response;
        response["buffer_pool"]["hits"] = pool.hits;
        response["buffer_pool"]["misses"] = pool.misses;
        response["buffer_pool"]["evictions"] = pool.evictions;
        response["buffer_pool"]["dirty_write_backs"] = pool.dirty_write_backs;
        response["buffer_pool"]["waits"] = pool.waits;
        response["buffer_pool"]["pinned_frames"] = bpm->pinned_frames();
        response["disk"]["reads"] = disk.reads;
        response["disk"]["writes"] = disk.writes;
        response["disk"]["syncs"] = disk.syncs;
        response["disk"]["bytes"] = disk.bytes;
        response["requests"] =
            app.get_middleware<ApiMiddleware>().requests.load(std::memory_order_relaxed);
        response["queries"] =
            app.get_middleware<ApiMiddleware>().queries.load(std::memory_order_relaxed);
        response["errors"] =
            app.get_middleware<ApiMiddleware>().errors.load(std::memory_order_relaxed);
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/api/metrics").methods(crow::HTTPMethod::GET)([this]() {
        const BufferPoolStats stats = bpm->SnapshotStats();
        const uint64_t queries =
            app.get_middleware<ApiMiddleware>().queries.load(std::memory_order_relaxed);
        const uint64_t elapsed =
            app.get_middleware<ApiMiddleware>().query_latency_us.load(std::memory_order_relaxed);
        const uint64_t errors =
            app.get_middleware<ApiMiddleware>().errors.load(std::memory_order_relaxed);
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
            queries == 0U ? 0.0 : static_cast<double>(elapsed) / queries / 1000.0;
        response["error_count"] = errors;
        response["source"] = "live";
        response["deprecated"] = true;
        return JsonResponse(200, response);
    });
    CROW_ROUTE(app, "/metrics").methods(crow::HTTPMethod::GET)([this]() {
        const BufferPoolStats pool = bpm->SnapshotStats();
        const DiskStats disk = disk_manager->Stats();
        const ApiMiddleware& middleware = app.get_middleware<ApiMiddleware>();
        std::ostringstream text;
        text << "# HELP reldb_buffer_pool_hits_total Buffer pool cache hits.\n"
             << "# TYPE reldb_buffer_pool_hits_total counter\n"
             << "reldb_buffer_pool_hits_total " << pool.hits << '\n'
             << "# HELP reldb_buffer_pool_misses_total Buffer pool cache misses.\n"
             << "# TYPE reldb_buffer_pool_misses_total counter\n"
             << "reldb_buffer_pool_misses_total " << pool.misses << '\n'
             << "# HELP reldb_disk_reads_total Disk page reads.\n"
             << "# TYPE reldb_disk_reads_total counter\n"
             << "reldb_disk_reads_total " << disk.reads << '\n'
             << "# HELP reldb_disk_writes_total Disk page writes.\n"
             << "# TYPE reldb_disk_writes_total counter\n"
             << "reldb_disk_writes_total " << disk.writes << '\n'
             << "# HELP reldb_http_requests_total HTTP requests handled.\n"
             << "# TYPE reldb_http_requests_total counter\n"
             << "reldb_http_requests_total " << middleware.requests.load(std::memory_order_relaxed)
             << '\n'
             << "# HELP reldb_http_errors_total HTTP error responses.\n"
             << "# TYPE reldb_http_errors_total counter\n"
             << "reldb_http_errors_total " << middleware.errors.load(std::memory_order_relaxed)
             << '\n'
             << "# HELP reldb_api_queries_total API SQL query requests.\n"
             << "# TYPE reldb_api_queries_total counter\n"
             << "reldb_api_queries_total " << middleware.queries.load(std::memory_order_relaxed)
             << '\n';
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
}

}  // namespace relational_db
