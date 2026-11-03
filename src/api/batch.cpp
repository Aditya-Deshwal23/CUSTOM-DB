#include "internal.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "metrics/metrics.h"

namespace relational_db {
namespace {

ApiMiddleware::context& RequestContext(crow::App<ApiMiddleware>& app,
                                       const crow::request& request) {
    return app.get_context<ApiMiddleware>(request);
}

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

std::string TrimSql(std::string_view sql) {
    while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.front())) != 0) {
        sql.remove_prefix(1);
    }
    while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.back())) != 0) {
        sql.remove_suffix(1);
    }
    return std::string(sql);
}

std::vector<std::string> SplitBatchScript(const std::string& script, bool& rolled_back) {
    const std::vector<SqlToken> tokens = SqlLexer::Tokenize(script);
    std::vector<std::string> statements;
    size_t segment_start = 0;
    bool began = false;
    bool ended = false;
    for (const SqlToken& token : tokens) {
        if (token.kind != SqlTokenKind::SEMICOLON) {
            continue;
        }
        const size_t semicolon_offset = static_cast<size_t>(token.text.data() - script.data());
        const std::string statement = TrimSql(
            std::string_view(script).substr(segment_start, semicolon_offset - segment_start));
        segment_start = semicolon_offset + token.text.size();
        if (statement.empty()) {
            continue;
        }
        if (!began) {
            if (!EqualsIgnoreCase(statement, "BEGIN")) {
                throw std::invalid_argument("script must start with BEGIN;");
            }
            began = true;
            continue;
        }
        if (ended) {
            throw std::invalid_argument("script transaction controls are malformed");
        }
        if (EqualsIgnoreCase(statement, "COMMIT")) {
            ended = true;
            continue;
        }
        if (EqualsIgnoreCase(statement, "ROLLBACK")) {
            ended = true;
            rolled_back = true;
            continue;
        }
        statements.push_back(statement);
        if (statements.size() > 50U) {
            throw std::length_error("batch contains more than 50 statements");
        }
    }
    if (!TrimSql(std::string_view(script).substr(segment_start)).empty() || !began || !ended) {
        throw std::invalid_argument("script must end with COMMIT; or ROLLBACK;");
    }
    if (!rolled_back && statements.empty()) {
        throw std::invalid_argument("batch must contain at least one SQL statement");
    }
    return statements;
}

std::string BatchErrorCode(const DbError& error) {
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
        case ErrorCode::QUERY_TIMEOUT:
            return "QUERY_TIMEOUT";
        case ErrorCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        default:
            return "INTERNAL";
    }
}

int BatchHttpStatus(const DbError& error) {
    switch (error.Code()) {
        case ErrorCode::UNKNOWN_TABLE:
            return 404;
        case ErrorCode::DUPLICATE_KEY:
        case ErrorCode::NOT_NULL_VIOLATION:
        case ErrorCode::FK_VIOLATION:
            return 409;
        case ErrorCode::BUSY:
        case ErrorCode::QUERY_TIMEOUT:
            return 503;
        case ErrorCode::INVALID_ARGUMENT:
        case ErrorCode::UNKNOWN_COLUMN:
            return 422;
        case ErrorCode::TYPE_MISMATCH:
            return 400;
        default:
            return 500;
    }
}

std::string BatchSqlErrorCode(const SqlError& error) {
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

int BatchSqlHttpStatus(const SqlError& error) {
    switch (error.Code()) {
        case SqlErrorCode::UNKNOWN_TABLE:
            return 404;
        case SqlErrorCode::DUPLICATE_KEY:
        case SqlErrorCode::NOT_NULL_VIOLATION:
        case SqlErrorCode::FK_VIOLATION:
            return 409;
        case SqlErrorCode::UNKNOWN_COLUMN:
            return 422;
        default:
            return 400;
    }
}

bool BatchReadOnly(SqlStatementKind kind) {
    return kind == SqlStatementKind::SELECT || kind == SqlStatementKind::SHOW_TABLES ||
           kind == SqlStatementKind::DESCRIBE || kind == SqlStatementKind::EXPLAIN;
}

crow::response BatchError(int status, std::string code, std::string message,
                          const std::string& request_id, size_t statement_index,
                          size_t line = 0, size_t column = 0, std::string hint = {}) {
    MetricsRegistry::Instance().RecordError(MetricErrorFromCode(code));
    crow::json::wvalue response;
    response["ok"] = false;
    response["error"]["code"] = std::move(code);
    response["error"]["message"] = std::move(message);
    response["error"]["statement_index"] = static_cast<uint64_t>(statement_index);
    response["error"]["line"] = line;
    response["error"]["column"] = column;
    response["error"]["hint"] = std::move(hint);
    response["request_id"] = request_id;
    crow::response result = JsonResponse(status, response);
    if (status == 503) {
        result.add_header("Retry-After", "1");
    }
    return result;
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

}  // namespace

crow::response HandleBatch(ApiServer::Impl& server, const crow::request& request) {
    ApiMiddleware& middleware = server.app.get_middleware<ApiMiddleware>();
    middleware.queries.fetch_add(1, std::memory_order_relaxed);
    auto& context = RequestContext(server.app, request);
    const std::string& request_id = context.request_id;
    const crow::json::rvalue input = crow::json::load(request.body);
    if (!input || input.t() != crow::json::type::Object) {
        return ErrorResponse(400, "INVALID_JSON", "Request body must be a JSON object",
                             request_id);
    }

    std::vector<std::string> queries;
    bool rolled_back = false;
    try {
        if (input.has("statements") && input.has("script")) {
            throw std::invalid_argument("provide either statements or script, not both");
        }
        if (input.has("statements") && input["statements"].t() == crow::json::type::List) {
            const crow::json::rvalue& statements = input["statements"];
            if (statements.size() == 0U) {
                throw std::invalid_argument("statements must not be empty");
            }
            if (statements.size() > 50U) {
                return ErrorResponse(413, "BATCH_TOO_LARGE",
                                     "A batch may contain at most 50 statements", request_id);
            }
            for (size_t index = 0; index < statements.size(); ++index) {
                const crow::json::rvalue& item = statements[index];
                if (item.t() != crow::json::type::Object || !item.has("sql") ||
                    item["sql"].t() != crow::json::type::String) {
                    throw std::invalid_argument("each statement must contain a string sql field");
                }
                const std::string sql = item["sql"].s();
                if (sql.empty() || sql.size() > server.config.max_sql_bytes) {
                    throw std::length_error("statement SQL is empty or exceeds MAX_SQL_BYTES");
                }
                crow::json::rvalue empty_params = crow::json::load("[]");
                const crow::json::rvalue& params = item.has("params") ? item["params"] : empty_params;
                std::string bound = BindParameters(sql, params);
                if (bound.size() > server.config.max_sql_bytes) {
                    throw std::length_error("bound statement exceeds MAX_SQL_BYTES");
                }
                queries.push_back(std::move(bound));
            }
        } else if (input.has("script") && input["script"].t() == crow::json::type::String) {
            const std::string script = input["script"].s();
            if (script.size() > server.config.max_sql_bytes) {
                return ErrorResponse(413, "SQL_TOO_LARGE", "Script exceeds MAX_SQL_BYTES",
                                     request_id);
            }
            queries = SplitBatchScript(script, rolled_back);
        } else {
            throw std::invalid_argument("provide statements array or transaction script");
        }
    } catch (const std::length_error& error) {
        const std::string message(error.what());
        const bool too_many = message.find("50 statements") != std::string::npos;
        return ErrorResponse(413, too_many ? "BATCH_TOO_LARGE" : "SQL_TOO_LARGE", message,
                             request_id);
    } catch (const SqlError& error) {
        return ErrorResponse(400, "SYNTAX_ERROR", error.what(), request_id, error.Line(),
                             error.Column(), error.Hint());
    } catch (const std::invalid_argument& error) {
        return ErrorResponse(400, "INVALID_BATCH", error.what(), request_id);
    }

    if (rolled_back) {
        crow::json::wvalue response;
        response["ok"] = true;
        response["committed"] = false;
        response["commit"]["status"] = "rolled_back";
        response["commit"]["durable"] = false;
        response["results"] = crow::json::wvalue::list{};
        return JsonResponse(200, response);
    }

    bool has_write = false;
    std::vector<SqlStatement> parsed_statements;
    parsed_statements.reserve(queries.size());
    for (size_t index = 0; index < queries.size(); ++index) {
        try {
            SqlStatement statement = SqlParser(queries[index]).Parse();
            has_write =
                has_write ||
                !(statement.kind == SqlStatementKind::EXPLAIN && statement.explained_statement
                      ? statement.explained_statement->kind == SqlStatementKind::SELECT
                      : BatchReadOnly(statement.kind));
            parsed_statements.push_back(std::move(statement));
        } catch (const SqlError& error) {
            return BatchError(BatchSqlHttpStatus(error), BatchSqlErrorCode(error), error.what(),
                              request_id, index, error.Line(), error.Column(), error.Hint());
        }
    }
    if (server.draining.load(std::memory_order_acquire) ||
        server.recovering.load(std::memory_order_acquire)) {
        return ErrorResponse(503, "UNAVAILABLE", "Service is recovering or draining", request_id);
    }
    if (has_write && !server.config.api_key.empty() &&
        !ApiKeyMatches(server.config.api_key, request.get_header_value("X-API-Key"))) {
        return ErrorResponse(401, "API_KEY_REQUIRED", "A valid X-API-Key is required", request_id);
    }
    if (server.config.read_only && has_write) {
        return ErrorResponse(403, "READ_ONLY", "Only read statements are allowed", request_id);
    }
    if (has_write && server.config.max_db_bytes != 0U &&
        server.disk_manager->SizeInPages() >
            server.config.max_db_bytes / static_cast<uint64_t>(PAGE_SIZE)) {
        return ErrorResponse(507, "STORAGE_QUOTA", "Database storage quota exceeded", request_id);
    }

    size_t failed_statement_index = 0;
    try {
        const std::vector<SqlExecutionResult> results = server.executor->ExecuteBatch(
            queries, std::chrono::milliseconds(server.config.statement_timeout_ms),
            server.config.max_result_rows, failed_statement_index);
        crow::json::wvalue response;
        response["ok"] = true;
        response["committed"] = true;
        response["commit"]["status"] = "committed";
        response["commit"]["durable"] = true;
        if (WriteAheadLog* wal = server.executor->Wal(); wal != nullptr) {
            response["commit"]["wal_commit_count"] = wal->CommitCount();
        }
        crow::json::wvalue::list json_results;
        for (const SqlExecutionResult& result : results) {
            crow::json::wvalue item;
            item["statement"] = result.statement;
            item["message"] = result.message;
            item["rows_affected"] = result.rows_affected;
            item["truncated"] = result.truncated;
            crow::json::wvalue::list columns;
            for (size_t index = 0; index < result.column_names.size(); ++index) {
                crow::json::wvalue column;
                column["name"] = result.column_names[index];
                column["type"] = ColumnTypeName(result.columns[index].type);
                columns.push_back(std::move(column));
            }
            item["columns"] = std::move(columns);
            crow::json::wvalue::list rows;
            for (const Row& row : result.rows) {
                crow::json::wvalue::list values;
                for (const Value& value : row) {
                    values.push_back(JsonValue(value));
                }
                rows.push_back(std::move(values));
            }
            item["rows"] = std::move(rows);
            item["row_count"] = static_cast<uint64_t>(result.rows.size());
            json_results.push_back(std::move(item));
        }
        response["results"] = std::move(json_results);
        return JsonResponse(200, response);
    } catch (const DbError& error) {
        return BatchError(BatchHttpStatus(error), BatchErrorCode(error), error.what(), request_id,
                          failed_statement_index);
    } catch (const std::exception&) {
        return BatchError(500, "INTERNAL", "Internal server error", request_id,
                          failed_statement_index);
    }
}

}  // namespace relational_db
