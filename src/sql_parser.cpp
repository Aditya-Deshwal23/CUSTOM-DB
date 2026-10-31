#include "sql_parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <utility>

namespace relational_db {
namespace {

bool IsIdentifierStart(char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalpha(character) != 0 || value == '_';
}

bool IsIdentifierPart(char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalnum(character) != 0 || value == '_' || value == '$';
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
        return std::tolower(static_cast<unsigned char>(lhs)) ==
               std::tolower(static_cast<unsigned char>(rhs));
    });
}

std::string Upper(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    });
    return result;
}

constexpr std::string_view kNotBetween = "NOT BETWEEN";
constexpr std::string_view kNotIn = "NOT IN";
constexpr std::string_view kNotLike = "NOT LIKE";

class SqlPrinter {
   public:
    std::string Print(const SqlStatement& statement) {
        switch (statement.kind) {
            case SqlStatementKind::CREATE_TABLE:
                PrintCreateTable(statement);
                break;
            case SqlStatementKind::DROP_TABLE:
                output_ = "DROP TABLE " + std::string(statement.table);
                break;
            case SqlStatementKind::CREATE_INDEX:
                output_ = statement.flag ? "CREATE UNIQUE INDEX " : "CREATE INDEX ";
                output_ += std::string(statement.name) + " ON " + std::string(statement.table);
                PrintNames(statement.index_columns);
                break;
            case SqlStatementKind::INSERT:
                PrintInsert(statement);
                break;
            case SqlStatementKind::SELECT:
                PrintSelect(statement);
                break;
            case SqlStatementKind::UPDATE:
                PrintUpdate(statement);
                break;
            case SqlStatementKind::DELETE:
                output_ = "DELETE FROM " + std::string(statement.table);
                PrintWhere(statement.where);
                break;
            case SqlStatementKind::EXPLAIN:
                output_ = statement.flag ? "EXPLAIN ANALYZE " : "EXPLAIN ";
                if (statement.explained_statement) {
                    output_ += SqlPrinter().Print(*statement.explained_statement);
                }
                break;
            case SqlStatementKind::SHOW_TABLES:
                output_ = "SHOW TABLES";
                break;
            case SqlStatementKind::DESCRIBE:
                output_ = "DESCRIBE " + std::string(statement.table);
                break;
            case SqlStatementKind::CHECKPOINT:
                output_ = "CHECKPOINT";
                break;
        }
        return output_;
    }

   private:
    static std::string PrintExpression(const SqlExpression& expression) {
        const auto child = [&](size_t index) {
            return PrintExpression(expression.children.at(index));
        };
        switch (expression.kind) {
            case SqlExpressionKind::EMPTY:
                return {};
            case SqlExpressionKind::LITERAL:
            case SqlExpressionKind::PARAMETER:
                return std::string(expression.value);
            case SqlExpressionKind::COLUMN:
                return expression.qualifier.empty()
                           ? std::string(expression.name)
                           : std::string(expression.qualifier) + "." + std::string(expression.name);
            case SqlExpressionKind::STAR:
                return "*";
            case SqlExpressionKind::UNARY:
                return "(" + Upper(expression.operation) +
                       (EqualsIgnoreCase(expression.operation, "NOT") ? " " : "") + child(0) + ")";
            case SqlExpressionKind::BINARY:
                return "(" + child(0) + " " + Upper(expression.operation) + " " + child(1) + ")";
            case SqlExpressionKind::IS_NULL:
                return "(" + child(0) + " IS " +
                       (EqualsIgnoreCase(expression.operation, "IS NOT") ? "NOT " : "") + "NULL)";
            case SqlExpressionKind::BETWEEN:
                return "(" + child(0) + " " + Upper(expression.operation) + " " + child(1) +
                       " AND " + child(2) + ")";
            case SqlExpressionKind::IN: {
                std::string result = "(" + child(0) + " " + Upper(expression.operation) + " (";
                for (size_t index = 1; index < expression.children.size(); ++index) {
                    if (index != 1)
                        result += ", ";
                    result += child(index);
                }
                return result + "))";
            }
            case SqlExpressionKind::FUNCTION: {
                std::string result = std::string(expression.name) + "(";
                for (size_t index = 0; index < expression.children.size(); ++index) {
                    if (index != 0)
                        result += ", ";
                    result += child(index);
                }
                return result + ")";
            }
        }
        return {};
    }

    void PrintNames(const std::vector<std::string_view>& names) {
        output_ += "(";
        for (size_t index = 0; index < names.size(); ++index) {
            if (index != 0)
                output_ += ", ";
            output_ += names[index];
        }
        output_ += ")";
    }

    void PrintCreateTable(const SqlStatement& statement) {
        output_ = "CREATE TABLE ";
        if (statement.if_not_exists)
            output_ += "IF NOT EXISTS ";
        output_ += std::string(statement.table) + " (";
        bool has_item = false;
        for (const SqlColumnDefinition& column : statement.column_definitions) {
            if (has_item)
                output_ += ", ";
            has_item = true;
            output_ += std::string(column.name) + " " + std::string(column.type_name);
            if (column.type_length)
                output_ += "(" + std::to_string(*column.type_length) + ")";
            for (const SqlConstraintKind constraint : column.constraints) {
                switch (constraint) {
                    case SqlConstraintKind::PRIMARY_KEY:
                        output_ += " PRIMARY KEY";
                        break;
                    case SqlConstraintKind::NOT_NULL:
                        output_ += " NOT NULL";
                        break;
                    case SqlConstraintKind::UNIQUE:
                        output_ += " UNIQUE";
                        break;
                    case SqlConstraintKind::REFERENCES:
                        output_ += " REFERENCES " + std::string(column.referenced_table) + "(" +
                                   std::string(column.referenced_column) + ")";
                        break;
                }
            }
        }
        if (!statement.primary_key_columns.empty()) {
            if (has_item)
                output_ += ", ";
            output_ += "PRIMARY KEY";
            PrintNames(statement.primary_key_columns);
        }
        output_ += ")";
    }

    void PrintInsert(const SqlStatement& statement) {
        output_ = "INSERT INTO " + std::string(statement.table);
        if (!statement.columns.empty()) {
            output_ += " ";
            PrintNames(statement.columns);
        }
        output_ += " VALUES ";
        for (size_t row_index = 0; row_index < statement.value_rows.size(); ++row_index) {
            if (row_index != 0)
                output_ += ", ";
            output_ += "(";
            const auto& row = statement.value_rows[row_index];
            for (size_t column_index = 0; column_index < row.size(); ++column_index) {
                if (column_index != 0)
                    output_ += ", ";
                output_ += PrintExpression(row[column_index]);
            }
            output_ += ")";
        }
    }

    void PrintSelect(const SqlStatement& statement) {
        output_ = "SELECT ";
        for (size_t index = 0; index < statement.projections.size(); ++index) {
            if (index != 0)
                output_ += ", ";
            const SqlSelectItem& projection = statement.projections[index];
            output_ += projection.star ? "*" : PrintExpression(projection.expression);
            if (!projection.alias.empty())
                output_ += " AS " + std::string(projection.alias);
        }
        output_ += " FROM " + std::string(statement.table);
        if (!statement.alias.empty())
            output_ += " " + std::string(statement.alias);
        PrintWhere(statement.where);
        if (!statement.group_by.empty()) {
            output_ += " GROUP BY ";
            for (size_t index = 0; index < statement.group_by.size(); ++index) {
                if (index != 0)
                    output_ += ", ";
                output_ += PrintExpression(statement.group_by[index]);
            }
        }
        if (!statement.order_by.empty()) {
            output_ += " ORDER BY ";
            for (size_t index = 0; index < statement.order_by.size(); ++index) {
                if (index != 0)
                    output_ += ", ";
                output_ += PrintExpression(statement.order_by[index].expression);
                output_ += statement.order_by[index].descending ? " DESC" : " ASC";
            }
        }
        if (statement.limit)
            output_ += " LIMIT " + std::to_string(*statement.limit);
        if (statement.offset != 0)
            output_ += " OFFSET " + std::to_string(statement.offset);
    }

    void PrintUpdate(const SqlStatement& statement) {
        output_ = "UPDATE " + std::string(statement.table) + " SET ";
        for (size_t index = 0; index < statement.assignments.size(); ++index) {
            if (index != 0)
                output_ += ", ";
            const SqlAssignment& assignment = statement.assignments[index];
            output_ +=
                std::string(assignment.column) + " = " + PrintExpression(assignment.expression);
        }
        PrintWhere(statement.where);
    }

    void PrintWhere(const SqlExpression& expression) {
        if (expression.kind != SqlExpressionKind::EMPTY) {
            output_ += " WHERE " + PrintExpression(expression);
        }
    }

    std::string output_;
};

}  // namespace

SqlError::SqlError(SqlErrorCode code, std::string message, size_t line, size_t column,
                   std::string hint)
    : std::runtime_error(std::move(message)),
      code_(code),
      line_(line),
      column_(column),
      hint_(std::move(hint)) {}

std::vector<SqlToken> SqlLexer::Tokenize(std::string_view query) {
    std::vector<SqlToken> tokens;
    size_t position = 0;
    size_t line = 1;
    size_t column = 1;
    const auto advance = [&](char character, size_t& current_line, size_t& current_column) {
        if (character == '\n') {
            ++current_line;
            current_column = 1;
        } else {
            ++current_column;
        }
    };
    const auto add = [&](SqlTokenKind kind, size_t start, size_t length, size_t token_line,
                         size_t token_column) {
        tokens.push_back({kind, query.substr(start, length), token_line, token_column});
    };
    while (position < query.size()) {
        const char character = query[position];
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            advance(character, line, column);
            ++position;
            continue;
        }
        if (character == '-' && position + 1 < query.size() && query[position + 1] == '-') {
            while (position < query.size() && query[position] != '\n') {
                advance(query[position], line, column);
                ++position;
            }
            continue;
        }
        if (character == '/' && position + 1 < query.size() && query[position + 1] == '*') {
            const size_t start_line = line;
            const size_t start_column = column;
            advance(query[position++], line, column);
            advance(query[position++], line, column);
            bool closed = false;
            while (position < query.size()) {
                if (query[position] == '*' && position + 1 < query.size() &&
                    query[position + 1] == '/') {
                    advance(query[position++], line, column);
                    advance(query[position++], line, column);
                    closed = true;
                    break;
                }
                advance(query[position], line, column);
                ++position;
            }
            if (!closed) {
                throw SqlError(SqlErrorCode::SYNTAX_ERROR, "Unterminated block comment", start_line,
                               start_column);
            }
            continue;
        }

        const size_t start = position;
        const size_t token_line = line;
        const size_t token_column = column;
        SqlTokenKind kind = SqlTokenKind::END;
        if (IsIdentifierStart(character)) {
            kind = SqlTokenKind::IDENTIFIER;
            do {
                advance(query[position], line, column);
                ++position;
            } while (position < query.size() && IsIdentifierPart(query[position]));
        } else if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
            kind = SqlTokenKind::INTEGER;
            while (position < query.size() &&
                   std::isdigit(static_cast<unsigned char>(query[position])) != 0) {
                advance(query[position++], line, column);
            }
            if (position < query.size() && query[position] == '.') {
                kind = SqlTokenKind::FLOAT;
                advance(query[position++], line, column);
                while (position < query.size() &&
                       std::isdigit(static_cast<unsigned char>(query[position])) != 0) {
                    advance(query[position++], line, column);
                }
            }
            if (position < query.size() && (query[position] == 'e' || query[position] == 'E')) {
                const size_t exponent = position;
                size_t exponent_position = position + 1;
                if (exponent_position < query.size() &&
                    (query[exponent_position] == '+' || query[exponent_position] == '-')) {
                    ++exponent_position;
                }
                if (exponent_position < query.size() &&
                    std::isdigit(static_cast<unsigned char>(query[exponent_position])) != 0) {
                    kind = SqlTokenKind::FLOAT;
                    while (position <= exponent_position) {
                        advance(query[position], line, column);
                        ++position;
                    }
                    while (position < query.size() &&
                           std::isdigit(static_cast<unsigned char>(query[position])) != 0) {
                        advance(query[position++], line, column);
                    }
                } else {
                    position = exponent;
                }
            }
        } else if (character == '.' && position + 1 < query.size() &&
                   std::isdigit(static_cast<unsigned char>(query[position + 1])) != 0) {
            kind = SqlTokenKind::FLOAT;
            advance(query[position++], line, column);
            while (position < query.size() &&
                   std::isdigit(static_cast<unsigned char>(query[position])) != 0) {
                advance(query[position++], line, column);
            }
            if (position < query.size() && (query[position] == 'e' || query[position] == 'E')) {
                const size_t exponent = position;
                size_t exponent_position = position + 1;
                if (exponent_position < query.size() &&
                    (query[exponent_position] == '+' || query[exponent_position] == '-')) {
                    ++exponent_position;
                }
                if (exponent_position < query.size() &&
                    std::isdigit(static_cast<unsigned char>(query[exponent_position])) != 0) {
                    while (position <= exponent_position) {
                        advance(query[position], line, column);
                        ++position;
                    }
                    while (position < query.size() &&
                           std::isdigit(static_cast<unsigned char>(query[position])) != 0) {
                        advance(query[position++], line, column);
                    }
                } else {
                    position = exponent;
                }
            }
        } else if (character == '\'') {
            kind = SqlTokenKind::STRING;
            advance(query[position++], line, column);
            bool closed = false;
            while (position < query.size()) {
                const char value = query[position];
                advance(value, line, column);
                ++position;
                if (value == '\'') {
                    if (position < query.size() && query[position] == '\'') {
                        advance(query[position++], line, column);
                    } else {
                        closed = true;
                        break;
                    }
                }
            }
            if (!closed) {
                throw SqlError(SqlErrorCode::SYNTAX_ERROR, "Unterminated string literal",
                               token_line, token_column);
            }
        } else if (character == '$') {
            kind = SqlTokenKind::PARAMETER;
            advance(query[position++], line, column);
            const size_t digits = position;
            while (position < query.size() &&
                   std::isdigit(static_cast<unsigned char>(query[position])) != 0) {
                advance(query[position++], line, column);
            }
            if (digits == position) {
                throw SqlError(SqlErrorCode::SYNTAX_ERROR,
                               "Parameter placeholder must have a numeric index", token_line,
                               token_column);
            }
        } else {
            const auto one = [&](SqlTokenKind token_kind) {
                kind = token_kind;
                advance(query[position++], line, column);
            };
            switch (character) {
                case '(':
                    one(SqlTokenKind::LEFT_PAREN);
                    break;
                case ')':
                    one(SqlTokenKind::RIGHT_PAREN);
                    break;
                case ',':
                    one(SqlTokenKind::COMMA);
                    break;
                case '.':
                    one(SqlTokenKind::DOT);
                    break;
                case '*':
                    one(SqlTokenKind::STAR);
                    break;
                case '+':
                    one(SqlTokenKind::PLUS);
                    break;
                case '-':
                    one(SqlTokenKind::MINUS);
                    break;
                case '/':
                    one(SqlTokenKind::SLASH);
                    break;
                case '%':
                    one(SqlTokenKind::PERCENT);
                    break;
                case ';':
                    one(SqlTokenKind::SEMICOLON);
                    break;
                case '=':
                    one(SqlTokenKind::EQUAL);
                    break;
                case '!':
                case '<':
                case '>': {
                    advance(query[position++], line, column);
                    if (position < query.size() && query[position] == '=') {
                        advance(query[position++], line, column);
                        kind = character == '<'   ? SqlTokenKind::LESS_EQUAL
                               : character == '>' ? SqlTokenKind::GREATER_EQUAL
                                                  : SqlTokenKind::NOT_EQUAL;
                    } else if (character == '<' && position < query.size() &&
                               query[position] == '>') {
                        advance(query[position++], line, column);
                        kind = SqlTokenKind::NOT_EQUAL;
                    } else if (character == '<') {
                        kind = SqlTokenKind::LESS;
                    } else if (character == '>') {
                        kind = SqlTokenKind::GREATER;
                    } else {
                        throw SqlError(SqlErrorCode::SYNTAX_ERROR, "Unexpected '!'", token_line,
                                       token_column, "Use '!=' for inequality.");
                    }
                    break;
                }
                default:
                    throw SqlError(SqlErrorCode::SYNTAX_ERROR, "Unexpected character", token_line,
                                   token_column);
            }
        }
        add(kind, start, position - start, token_line, token_column);
    }
    tokens.push_back({SqlTokenKind::END, {}, line, column});
    return tokens;
}

std::string SqlLexer::DecodeString(const SqlToken& token) {
    if (token.kind != SqlTokenKind::STRING || token.text.size() < 2) {
        throw SqlError(SqlErrorCode::INVALID_ARGUMENT, "Token is not a string literal", token.line,
                       token.column);
    }
    std::string value;
    value.reserve(token.text.size() - 2);
    for (size_t index = 1; index + 1 < token.text.size(); ++index) {
        if (token.text[index] == '\'' && index + 1 < token.text.size() - 1 &&
            token.text[index + 1] == '\'') {
            ++index;
        }
        value.push_back(token.text[index]);
    }
    return value;
}

SqlParser::SqlParser(std::string_view query) : tokens_(SqlLexer::Tokenize(query)) {}

SqlStatement SqlParser::Parse() {
    SqlStatement statement = ParseStatement();
    Match(SqlTokenKind::SEMICOLON);
    ExpectEnd();
    return statement;
}

SqlStatement SqlParser::ParseStatement() {
    if (MatchKeyword("CREATE"))
        return ParseCreate();
    if (MatchKeyword("INSERT"))
        return ParseInsert();
    if (MatchKeyword("SELECT"))
        return ParseSelect();
    if (MatchKeyword("UPDATE"))
        return ParseUpdate();
    if (MatchKeyword("DELETE"))
        return ParseDelete();
    if (MatchKeyword("EXPLAIN"))
        return ParseExplain();
    if (MatchKeyword("DROP")) {
        ExpectKeyword("TABLE");
        SqlStatement result;
        result.kind = SqlStatementKind::DROP_TABLE;
        result.table = ParseIdentifier();
        return result;
    }
    if (MatchKeyword("SHOW")) {
        ExpectKeyword("TABLES");
        SqlStatement result;
        result.kind = SqlStatementKind::SHOW_TABLES;
        return result;
    }
    if (MatchKeyword("DESCRIBE")) {
        SqlStatement result;
        result.kind = SqlStatementKind::DESCRIBE;
        result.table = ParseIdentifier();
        return result;
    }
    if (MatchKeyword("CHECKPOINT")) {
        SqlStatement result;
        result.kind = SqlStatementKind::CHECKPOINT;
        return result;
    }
    Error(Consume(), "Expected a SQL statement");
}

SqlStatement SqlParser::ParseCreate() {
    SqlStatement result;
    if (MatchKeyword("TABLE")) {
        result.kind = SqlStatementKind::CREATE_TABLE;
        if (MatchKeyword("IF")) {
            ExpectKeyword("NOT");
            ExpectKeyword("EXISTS");
            result.if_not_exists = true;
        }
        result.table = ParseIdentifier();
        Expect(SqlTokenKind::LEFT_PAREN, "'('");
        bool first = true;
        while (!Check(SqlTokenKind::RIGHT_PAREN)) {
            if (!first)
                Expect(SqlTokenKind::COMMA, "','");
            first = false;
            if (MatchKeyword("PRIMARY")) {
                ExpectKeyword("KEY");
                Expect(SqlTokenKind::LEFT_PAREN, "'('");
                result.primary_key_columns = ParseIdentifierList();
                Expect(SqlTokenKind::RIGHT_PAREN, "')'");
            } else {
                result.column_definitions.push_back(ParseColumnDefinition());
            }
        }
        Expect(SqlTokenKind::RIGHT_PAREN, "')'");
        return result;
    }
    if (MatchKeyword("UNIQUE")) {
        result.flag = true;
        ExpectKeyword("INDEX");
    } else {
        ExpectKeyword("INDEX");
    }
    result.kind = SqlStatementKind::CREATE_INDEX;
    result.name = ParseIdentifier();
    ExpectKeyword("ON");
    result.table = ParseIdentifier();
    Expect(SqlTokenKind::LEFT_PAREN, "'('");
    result.index_columns = ParseIdentifierList();
    Expect(SqlTokenKind::RIGHT_PAREN, "')'");
    return result;
}

SqlColumnDefinition SqlParser::ParseColumnDefinition() {
    SqlColumnDefinition result;
    result.name = ParseIdentifier();
    const SqlToken& type_token = Expect(SqlTokenKind::IDENTIFIER, "a column type");
    result.type_name = type_token.text;
    if (Match(SqlTokenKind::LEFT_PAREN)) {
        const uint64_t length = ParseUnsignedInteger();
        if (length > std::numeric_limits<uint16_t>::max()) {
            Error(tokens_[current_ - 1], "Declared type length is too large");
        }
        result.type_length = static_cast<uint16_t>(length);
        Expect(SqlTokenKind::RIGHT_PAREN, "')'");
    }
    while (true) {
        if (MatchKeyword("PRIMARY")) {
            ExpectKeyword("KEY");
            result.constraints.push_back(SqlConstraintKind::PRIMARY_KEY);
        } else if (MatchKeyword("NOT")) {
            ExpectKeyword("NULL");
            result.constraints.push_back(SqlConstraintKind::NOT_NULL);
        } else if (MatchKeyword("UNIQUE")) {
            result.constraints.push_back(SqlConstraintKind::UNIQUE);
        } else if (MatchKeyword("REFERENCES")) {
            result.constraints.push_back(SqlConstraintKind::REFERENCES);
            result.referenced_table = ParseIdentifier();
            Expect(SqlTokenKind::LEFT_PAREN, "'('");
            result.referenced_column = ParseIdentifier();
            Expect(SqlTokenKind::RIGHT_PAREN, "')'");
        } else {
            break;
        }
    }
    return result;
}

SqlStatement SqlParser::ParseInsert() {
    SqlStatement result;
    result.kind = SqlStatementKind::INSERT;
    ExpectKeyword("INTO");
    result.table = ParseIdentifier();
    if (Match(SqlTokenKind::LEFT_PAREN)) {
        result.columns = ParseIdentifierList();
        Expect(SqlTokenKind::RIGHT_PAREN, "')'");
    }
    ExpectKeyword("VALUES");
    do {
        Expect(SqlTokenKind::LEFT_PAREN, "'('");
        std::vector<SqlExpression> row;
        if (!Check(SqlTokenKind::RIGHT_PAREN)) {
            do {
                row.push_back(ParseExpression());
            } while (Match(SqlTokenKind::COMMA));
        }
        Expect(SqlTokenKind::RIGHT_PAREN, "')'");
        result.value_rows.push_back(std::move(row));
    } while (Match(SqlTokenKind::COMMA));
    return result;
}

SqlStatement SqlParser::ParseSelect() {
    SqlStatement result;
    result.kind = SqlStatementKind::SELECT;
    do {
        SqlSelectItem item;
        if (Match(SqlTokenKind::STAR)) {
            item.star = true;
        } else {
            item.expression = ParseExpression();
        }
        if (MatchKeyword("AS"))
            item.alias = ParseIdentifier();
        result.projections.push_back(std::move(item));
    } while (Match(SqlTokenKind::COMMA));
    ExpectKeyword("FROM");
    result.table = ParseIdentifier();
    if (MatchKeyword("AS")) {
        result.alias = ParseIdentifier();
    } else if (Check(SqlTokenKind::IDENTIFIER) && !CheckKeyword("WHERE") &&
               !CheckKeyword("GROUP") && !CheckKeyword("ORDER") && !CheckKeyword("LIMIT") &&
               !CheckKeyword("OFFSET")) {
        result.alias = ParseIdentifier();
    }
    if (MatchKeyword("WHERE"))
        result.where = ParseExpression();
    if (MatchKeyword("GROUP")) {
        ExpectKeyword("BY");
        do {
            result.group_by.push_back(ParseExpression());
        } while (Match(SqlTokenKind::COMMA));
    }
    if (MatchKeyword("ORDER")) {
        ExpectKeyword("BY");
        do {
            SqlOrderItem item;
            item.expression = ParseExpression();
            if (MatchKeyword("DESC")) {
                item.descending = true;
            } else {
                MatchKeyword("ASC");
            }
            result.order_by.push_back(std::move(item));
        } while (Match(SqlTokenKind::COMMA));
    }
    if (MatchKeyword("LIMIT"))
        result.limit = ParseUnsignedInteger();
    if (MatchKeyword("OFFSET"))
        result.offset = ParseUnsignedInteger();
    return result;
}

SqlStatement SqlParser::ParseUpdate() {
    SqlStatement result;
    result.kind = SqlStatementKind::UPDATE;
    result.table = ParseIdentifier();
    ExpectKeyword("SET");
    do {
        SqlAssignment assignment;
        assignment.column = ParseIdentifier();
        Expect(SqlTokenKind::EQUAL, "'='");
        assignment.expression = ParseExpression();
        result.assignments.push_back(std::move(assignment));
    } while (Match(SqlTokenKind::COMMA));
    if (MatchKeyword("WHERE"))
        result.where = ParseExpression();
    return result;
}

SqlStatement SqlParser::ParseDelete() {
    SqlStatement result;
    result.kind = SqlStatementKind::DELETE;
    ExpectKeyword("FROM");
    result.table = ParseIdentifier();
    if (MatchKeyword("WHERE"))
        result.where = ParseExpression();
    return result;
}

SqlStatement SqlParser::ParseExplain() {
    SqlStatement result;
    result.kind = SqlStatementKind::EXPLAIN;
    result.flag = MatchKeyword("ANALYZE");
    result.explained_statement = std::make_shared<SqlStatement>(ParseStatement());
    result.explained_kind = result.explained_statement->kind;
    return result;
}

SqlExpression SqlParser::ParseExpression(int minimum_precedence) {
    SqlExpression left = ParsePrefix();
    while (true) {
        bool negated = false;
        const SqlToken& token = tokens_[current_];
        int precedence = 0;
        std::string_view operation;
        if (CheckKeyword("OR")) {
            precedence = 1;
            operation = "OR";
        } else if (CheckKeyword("AND")) {
            precedence = 2;
            operation = "AND";
        } else if (CheckKeyword("NOT") && current_ + 1 < tokens_.size() &&
                   (EqualsIgnoreCase(tokens_[current_ + 1].text, "BETWEEN") ||
                    EqualsIgnoreCase(tokens_[current_ + 1].text, "IN") ||
                    EqualsIgnoreCase(tokens_[current_ + 1].text, "LIKE"))) {
            negated = true;
            precedence = 3;
            operation = tokens_[current_ + 1].text;
        } else if (CheckKeyword("IS") || CheckKeyword("BETWEEN") || CheckKeyword("IN") ||
                   CheckKeyword("LIKE")) {
            precedence = 3;
            operation = token.text;
        } else {
            switch (token.kind) {
                case SqlTokenKind::EQUAL:
                case SqlTokenKind::NOT_EQUAL:
                case SqlTokenKind::LESS:
                case SqlTokenKind::LESS_EQUAL:
                case SqlTokenKind::GREATER:
                case SqlTokenKind::GREATER_EQUAL:
                    precedence = 3;
                    operation = token.text;
                    break;
                case SqlTokenKind::PLUS:
                case SqlTokenKind::MINUS:
                    precedence = 4;
                    operation = token.text;
                    break;
                case SqlTokenKind::STAR:
                case SqlTokenKind::SLASH:
                case SqlTokenKind::PERCENT:
                    precedence = 5;
                    operation = token.text;
                    break;
                default:
                    break;
            }
        }
        if (precedence < minimum_precedence || precedence == 0)
            break;

        if (CheckKeyword("IS")) {
            Consume();
            const bool is_not = MatchKeyword("NOT");
            ExpectKeyword("NULL");
            SqlExpression expression;
            expression.kind = SqlExpressionKind::IS_NULL;
            expression.operation = is_not ? "IS NOT" : "IS";
            expression.children.push_back(std::move(left));
            left = std::move(expression);
            continue;
        }
        if (CheckKeyword("BETWEEN") || (negated && EqualsIgnoreCase(operation, "BETWEEN"))) {
            if (negated) {
                Consume();
                ExpectKeyword("BETWEEN");
            } else {
                Consume();
            }
            SqlExpression expression;
            expression.kind = SqlExpressionKind::BETWEEN;
            expression.operation = negated ? kNotBetween : "BETWEEN";
            expression.children.push_back(std::move(left));
            expression.children.push_back(ParseExpression(precedence + 1));
            ExpectKeyword("AND");
            expression.children.push_back(ParseExpression(precedence + 1));
            left = std::move(expression);
            continue;
        }
        if (CheckKeyword("IN") || (negated && EqualsIgnoreCase(operation, "IN"))) {
            if (negated) {
                Consume();
                ExpectKeyword("IN");
            } else {
                Consume();
            }
            SqlExpression expression;
            expression.kind = SqlExpressionKind::IN;
            expression.operation = negated ? kNotIn : "IN";
            expression.children.push_back(std::move(left));
            Expect(SqlTokenKind::LEFT_PAREN, "'('");
            do {
                expression.children.push_back(ParseExpression());
            } while (Match(SqlTokenKind::COMMA));
            Expect(SqlTokenKind::RIGHT_PAREN, "')'");
            left = std::move(expression);
            continue;
        }
        if (CheckKeyword("LIKE") || (negated && EqualsIgnoreCase(operation, "LIKE"))) {
            if (negated) {
                Consume();
                ExpectKeyword("LIKE");
            } else {
                Consume();
            }
            SqlExpression right = ParseExpression(precedence + 1);
            SqlExpression expression;
            expression.kind = SqlExpressionKind::BINARY;
            expression.operation = negated ? kNotLike : "LIKE";
            expression.children.push_back(std::move(left));
            expression.children.push_back(std::move(right));
            left = std::move(expression);
            continue;
        }

        Consume();
        SqlExpression right = ParseExpression(precedence + 1);
        SqlExpression expression;
        expression.kind = SqlExpressionKind::BINARY;
        expression.operation = operation;
        expression.children.push_back(std::move(left));
        expression.children.push_back(std::move(right));
        left = std::move(expression);
    }
    return left;
}

SqlExpression SqlParser::ParsePrefix() {
    if (CheckKeyword("NOT") || Check(SqlTokenKind::PLUS) || Check(SqlTokenKind::MINUS)) {
        const SqlToken& operation = Consume();
        SqlExpression expression;
        expression.kind = SqlExpressionKind::UNARY;
        expression.operation = EqualsIgnoreCase(operation.text, "NOT") ? "NOT" : operation.text;
        expression.children.push_back(
            ParseExpression(EqualsIgnoreCase(operation.text, "NOT") ? 3 : 6));
        return expression;
    }
    return ParsePrimary();
}

SqlExpression SqlParser::ParsePrimary() {
    if (Match(SqlTokenKind::LEFT_PAREN)) {
        SqlExpression expression = ParseExpression();
        Expect(SqlTokenKind::RIGHT_PAREN, "')'");
        return expression;
    }
    if (Check(SqlTokenKind::INTEGER) || Check(SqlTokenKind::FLOAT) || Check(SqlTokenKind::STRING) ||
        CheckKeyword("TRUE") || CheckKeyword("FALSE") || CheckKeyword("NULL")) {
        const SqlToken& token = Consume();
        SqlExpression expression;
        expression.kind = SqlExpressionKind::LITERAL;
        expression.value = token.text;
        return expression;
    }
    if (Match(SqlTokenKind::PARAMETER)) {
        SqlExpression expression;
        expression.kind = SqlExpressionKind::PARAMETER;
        expression.value = tokens_[current_ - 1].text;
        return expression;
    }
    if (Match(SqlTokenKind::STAR)) {
        SqlExpression expression;
        expression.kind = SqlExpressionKind::STAR;
        return expression;
    }
    if (Check(SqlTokenKind::IDENTIFIER)) {
        const SqlToken& identifier = Consume();
        if (Match(SqlTokenKind::LEFT_PAREN)) {
            SqlExpression expression;
            expression.kind = SqlExpressionKind::FUNCTION;
            expression.name = identifier.text;
            if (!Check(SqlTokenKind::RIGHT_PAREN)) {
                do {
                    expression.children.push_back(ParseExpression());
                } while (Match(SqlTokenKind::COMMA));
            }
            Expect(SqlTokenKind::RIGHT_PAREN, "')'");
            const bool count = EqualsIgnoreCase(expression.name, "COUNT") &&
                               expression.children.size() == 1 &&
                               expression.children.front().kind == SqlExpressionKind::STAR;
            const bool numeric_aggregate =
                (EqualsIgnoreCase(expression.name, "SUM") ||
                 EqualsIgnoreCase(expression.name, "MIN") ||
                 EqualsIgnoreCase(expression.name, "MAX") ||
                 EqualsIgnoreCase(expression.name, "AVG")) &&
                expression.children.size() == 1 &&
                expression.children.front().kind == SqlExpressionKind::COLUMN;
            if (!count && !numeric_aggregate) {
                Error(identifier, "Expected COUNT(*) or SUM/MIN/MAX/AVG(column)");
            }
            return expression;
        }
        SqlExpression expression;
        expression.kind = SqlExpressionKind::COLUMN;
        expression.name = identifier.text;
        if (Match(SqlTokenKind::DOT)) {
            expression.qualifier = expression.name;
            expression.name = ParseIdentifier();
        }
        return expression;
    }
    Error(tokens_[current_], "Expected an expression");
}

SqlExpression SqlParser::ParseColumnReference() {
    SqlExpression expression;
    expression.kind = SqlExpressionKind::COLUMN;
    expression.name = ParseIdentifier();
    if (Match(SqlTokenKind::DOT)) {
        expression.qualifier = expression.name;
        expression.name = ParseIdentifier();
    }
    return expression;
}

std::vector<std::string_view> SqlParser::ParseIdentifierList() {
    std::vector<std::string_view> result;
    do {
        result.push_back(ParseIdentifier());
    } while (Match(SqlTokenKind::COMMA));
    return result;
}

std::string_view SqlParser::ParseIdentifier() {
    return Expect(SqlTokenKind::IDENTIFIER, "an identifier").text;
}

uint64_t SqlParser::ParseUnsignedInteger() {
    const SqlToken& token = Expect(SqlTokenKind::INTEGER, "an unsigned integer");
    uint64_t value = 0;
    const auto parsed =
        std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.text.data() + token.text.size()) {
        Error(token, "Integer is outside the supported range");
    }
    return value;
}

bool SqlParser::MatchKeyword(std::string_view keyword) {
    if (!CheckKeyword(keyword))
        return false;
    ++current_;
    return true;
}

bool SqlParser::CheckKeyword(std::string_view keyword) const {
    return Check(SqlTokenKind::IDENTIFIER) && EqualsIgnoreCase(tokens_[current_].text, keyword);
}

bool SqlParser::Match(SqlTokenKind kind) {
    if (!Check(kind))
        return false;
    ++current_;
    return true;
}

bool SqlParser::Check(SqlTokenKind kind) const {
    return tokens_[current_].kind == kind;
}

const SqlToken& SqlParser::Consume() {
    const SqlToken& token = tokens_[current_];
    if (token.kind != SqlTokenKind::END)
        ++current_;
    return token;
}

const SqlToken& SqlParser::Expect(SqlTokenKind kind, std::string_view expectation) {
    if (!Check(kind))
        Error(tokens_[current_], std::string("Expected ") + std::string(expectation));
    return Consume();
}

void SqlParser::ExpectKeyword(std::string_view keyword) {
    if (!MatchKeyword(keyword)) {
        Error(tokens_[current_], std::string("Expected keyword ") + Upper(keyword));
    }
}

void SqlParser::ExpectEnd() {
    if (!Check(SqlTokenKind::END))
        Error(tokens_[current_], "Unexpected trailing token");
}

[[noreturn]] void SqlParser::Error(const SqlToken& token, std::string_view message) const {
    throw SqlError(SqlErrorCode::SYNTAX_ERROR, std::string(message), token.line, token.column);
}

std::string PrintSql(const SqlStatement& statement) {
    return SqlPrinter().Print(statement);
}

}  // namespace relational_db
