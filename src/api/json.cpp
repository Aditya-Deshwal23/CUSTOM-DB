#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "internal.h"
#include "metrics/metrics.h"

namespace relational_db {
namespace {

std::string SqlLiteral(const crow::json::rvalue& value) {
    switch (value.t()) {
        case crow::json::type::Null:
            return "NULL";
        case crow::json::type::True:
            return "TRUE";
        case crow::json::type::False:
            return "FALSE";
        case crow::json::type::Number:
            return static_cast<std::string>(value);
        case crow::json::type::String: {
            std::string literal = "'";
            for (char character : value.s()) {
                literal.push_back(character);
                if (character == '\'') {
                    literal.push_back('\'');
                }
            }
            literal.push_back('\'');
            return literal;
        }
        case crow::json::type::List:
        case crow::json::type::Object:
        case crow::json::type::Function:
            throw std::invalid_argument("SQL parameters must be scalar JSON values");
    }
    throw std::invalid_argument("Unsupported SQL parameter");
}

}  // namespace

crow::response JsonResponse(int status, const crow::json::wvalue& value) {
    crow::response response(status, value.dump());
    response.add_header("Content-Type", "application/json; charset=utf-8");
    return response;
}

crow::response ErrorResponse(int status, std::string code, std::string message,
                             const std::string& request_id, size_t line, size_t column,
                             std::string hint) {
    MetricsRegistry& metrics = MetricsRegistry::Instance();
    metrics.RecordError(MetricErrorFromCode(code));
    if (code == "BUSY" || code == "BUFFER_POOL_EXHAUSTED") {
        metrics.busy_rejections.Increment();
    }
    crow::json::wvalue details;
    details["code"] = std::move(code);
    details["message"] = std::move(message);
    details["line"] = line;
    details["column"] = column;
    details["hint"] = std::move(hint);

    crow::json::wvalue response;
    response["ok"] = false;
    response["error"] = std::move(details);
    response["request_id"] = request_id;
    return JsonResponse(status, response);
}

void ApplyResponseHeaders(crow::response& response) {
    response.add_header("X-Content-Type-Options", "nosniff");
    response.add_header("X-Frame-Options", "DENY");
    response.add_header("Referrer-Policy", "no-referrer");
    response.add_header("Content-Security-Policy", "default-src 'self'");
}

std::string BindParameters(const std::string& sql, const crow::json::rvalue& params) {
    if (params.t() != crow::json::type::List) {
        throw std::invalid_argument("params must be a JSON array");
    }

    struct Replacement {
        size_t offset;
        size_t length;
        std::string literal;
    };
    std::vector<Replacement> replacements;
    const std::vector<SqlToken> tokens = SqlLexer::Tokenize(sql);
    for (const SqlToken& token : tokens) {
        if (token.kind != SqlTokenKind::PARAMETER) {
            continue;
        }
        const std::string_view text = token.text;
        size_t parameter = 0;
        for (size_t index = 1; index < text.size(); ++index) {
            if (text[index] < '0' || text[index] > '9') {
                throw std::invalid_argument("Invalid SQL parameter number");
            }
            const size_t digit = static_cast<size_t>(text[index] - '0');
            if (parameter > (std::numeric_limits<size_t>::max() - digit) / 10U) {
                throw std::invalid_argument("SQL parameter number is too large");
            }
            parameter = parameter * 10U + digit;
            if (parameter > params.size()) {
                throw std::invalid_argument("SQL parameter has no matching value");
            }
        }
        if (parameter == 0) {
            throw std::invalid_argument("SQL parameters are numbered from $1");
        }
        const size_t offset = static_cast<size_t>(token.text.data() - sql.data());
        replacements.push_back(
            Replacement{offset, token.text.size(), SqlLiteral(params[parameter - 1U])});
    }
    if (replacements.empty() && params.size() != 0U) {
        throw std::invalid_argument("params supplied but SQL has no parameters");
    }

    std::sort(replacements.begin(), replacements.end(),
              [](const Replacement& left, const Replacement& right) {
                  return left.offset > right.offset;
              });
    std::string bound = sql;
    for (const Replacement& replacement : replacements) {
        bound.replace(replacement.offset, replacement.length, replacement.literal);
    }
    return bound;
}

std::string SqlStatementName(SqlStatementKind kind) {
    switch (kind) {
        case SqlStatementKind::CREATE_TABLE:
        case SqlStatementKind::CREATE_INDEX:
            return "CREATE";
        case SqlStatementKind::DROP_TABLE:
            return "DROP";
        case SqlStatementKind::INSERT:
            return "INSERT";
        case SqlStatementKind::SELECT:
            return "SELECT";
        case SqlStatementKind::UPDATE:
            return "UPDATE";
        case SqlStatementKind::DELETE:
            return "DELETE";
        case SqlStatementKind::EXPLAIN:
            return "EXPLAIN";
        case SqlStatementKind::SHOW_TABLES:
            return "SHOW";
        case SqlStatementKind::DESCRIBE:
            return "DESCRIBE";
        case SqlStatementKind::CHECKPOINT:
            return "CHECKPOINT";
    }
    return "UNKNOWN";
}

bool ApiKeyMatches(const std::string& expected, const std::string& supplied) {
    constexpr size_t kMaxApiKeyBytes = 4096;
    size_t difference = expected.size() ^ supplied.size();
    difference |= static_cast<size_t>(expected.size() > kMaxApiKeyBytes);
    difference |= static_cast<size_t>(supplied.size() > kMaxApiKeyBytes);
    for (size_t index = 0; index < kMaxApiKeyBytes; ++index) {
        const unsigned char expected_byte =
            index < expected.size() ? static_cast<unsigned char>(expected[index]) : 0U;
        const unsigned char supplied_byte =
            index < supplied.size() ? static_cast<unsigned char>(supplied[index]) : 0U;
        difference |= static_cast<size_t>(expected_byte ^ supplied_byte);
    }
    return difference == 0U && !expected.empty();
}

}  // namespace relational_db
