#ifndef RELDB_SQL_PARSER_H
#define RELDB_SQL_PARSER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace relational_db {

enum class SqlErrorCode {
    SYNTAX_ERROR,
    UNKNOWN_TABLE,
    UNKNOWN_COLUMN,
    TYPE_MISMATCH,
    DUPLICATE_KEY,
    NOT_NULL_VIOLATION,
    FK_VIOLATION,
    INVALID_ARGUMENT
};

class SqlError final : public std::runtime_error {
   public:
    SqlError(SqlErrorCode code, std::string message, size_t line, size_t column,
             std::string hint = {});

    SqlErrorCode Code() const noexcept {
        return code_;
    }
    size_t Line() const noexcept {
        return line_;
    }
    size_t Column() const noexcept {
        return column_;
    }
    const std::string& Hint() const noexcept {
        return hint_;
    }

   private:
    SqlErrorCode code_;
    size_t line_;
    size_t column_;
    std::string hint_;
};

enum class SqlTokenKind {
    END,
    IDENTIFIER,
    INTEGER,
    FLOAT,
    STRING,
    PARAMETER,
    LEFT_PAREN,
    RIGHT_PAREN,
    COMMA,
    DOT,
    STAR,
    PLUS,
    MINUS,
    SLASH,
    PERCENT,
    EQUAL,
    NOT_EQUAL,
    LESS,
    LESS_EQUAL,
    GREATER,
    GREATER_EQUAL,
    SEMICOLON
};

struct SqlToken {
    SqlTokenKind kind{SqlTokenKind::END};
    std::string_view text;
    size_t line{1};
    size_t column{1};
};

class SqlLexer {
   public:
    // Token text is a view into query; keep the source buffer alive while using tokens.
    static std::vector<SqlToken> Tokenize(std::string_view query);
    static std::string DecodeString(const SqlToken& token);
};

enum class SqlExpressionKind {
    EMPTY,
    LITERAL,
    PARAMETER,
    COLUMN,
    STAR,
    UNARY,
    BINARY,
    IS_NULL,
    BETWEEN,
    IN,
    FUNCTION
};

struct SqlExpression {
    SqlExpressionKind kind{SqlExpressionKind::EMPTY};
    std::string_view operation;
    std::string_view value;
    std::string_view qualifier;
    std::string_view name;
    std::vector<SqlExpression> children;
};

enum class SqlConstraintKind { PRIMARY_KEY, NOT_NULL, UNIQUE, REFERENCES };

struct SqlColumnDefinition {
    std::string_view name;
    std::string_view type_name;
    std::optional<uint16_t> type_length;
    std::vector<SqlConstraintKind> constraints;
    std::string_view referenced_table;
    std::string_view referenced_column;
};

enum class SqlStatementKind {
    CREATE_TABLE,
    DROP_TABLE,
    CREATE_INDEX,
    INSERT,
    SELECT,
    UPDATE,
    DELETE,
    EXPLAIN,
    SHOW_TABLES,
    DESCRIBE,
    CHECKPOINT
};

struct SqlSelectItem {
    bool star{false};
    SqlExpression expression;
    std::string_view alias;
};

struct SqlOrderItem {
    SqlExpression expression;
    bool descending{false};
};

struct SqlAssignment {
    std::string_view column;
    SqlExpression expression;
};

struct SqlStatement {
    SqlStatementKind kind{SqlStatementKind::SELECT};
    std::string_view table;
    std::string_view name;
    std::string_view alias;
    bool flag{false};
    bool if_not_exists{false};
    std::vector<SqlColumnDefinition> column_definitions;
    std::vector<std::string_view> columns;
    std::vector<SqlExpression> group_by;
    std::vector<SqlSelectItem> projections;
    std::vector<SqlOrderItem> order_by;
    std::vector<std::vector<SqlExpression>> value_rows;
    std::vector<SqlAssignment> assignments;
    SqlExpression where;
    std::optional<uint64_t> limit;
    uint64_t offset{0};
    std::vector<std::string_view> primary_key_columns;
    std::vector<std::string_view> index_columns;
    std::optional<SqlStatementKind> explained_kind;
    std::shared_ptr<SqlStatement> explained_statement;
};

class SqlParser {
   public:
    // AST names and literals are views into query; the source buffer must outlive the AST.
    explicit SqlParser(std::string_view query);
    SqlStatement Parse();

   private:
    SqlStatement ParseStatement();
    SqlStatement ParseCreate();
    SqlStatement ParseInsert();
    SqlStatement ParseSelect();
    SqlStatement ParseUpdate();
    SqlStatement ParseDelete();
    SqlStatement ParseExplain();
    SqlColumnDefinition ParseColumnDefinition();
    SqlExpression ParseExpression(int minimum_precedence = 1);
    SqlExpression ParsePrefix();
    SqlExpression ParsePrimary();
    SqlExpression ParseColumnReference();
    std::vector<std::string_view> ParseIdentifierList();
    std::string_view ParseIdentifier();
    uint64_t ParseUnsignedInteger();
    bool MatchKeyword(std::string_view keyword);
    bool CheckKeyword(std::string_view keyword) const;
    bool Match(SqlTokenKind kind);
    bool Check(SqlTokenKind kind) const;
    const SqlToken& Consume();
    const SqlToken& Expect(SqlTokenKind kind, std::string_view expectation);
    void ExpectKeyword(std::string_view keyword);
    void ExpectEnd();
    [[noreturn]] void Error(const SqlToken& token, std::string_view message) const;

    std::vector<SqlToken> tokens_;
    size_t current_{0};
};

std::string PrintSql(const SqlStatement& statement);

}  // namespace relational_db

#endif
