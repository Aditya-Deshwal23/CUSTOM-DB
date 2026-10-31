#include "sql_parser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace relational_db {
namespace {

TEST(SqlLexerTest, TokensAreViewsAndTrackSourcePositions) {
    const std::string query = "SELECT 'Ada''s'\nFROM people WHERE id >= $2";
    const std::vector<SqlToken> tokens = SqlLexer::Tokenize(query);

    ASSERT_GE(tokens.size(), 8U);
    EXPECT_EQ(tokens[0].kind, SqlTokenKind::IDENTIFIER);
    EXPECT_EQ(tokens[0].text, "SELECT");
    EXPECT_EQ(tokens[0].line, 1U);
    EXPECT_EQ(tokens[0].column, 1U);
    EXPECT_EQ(tokens[1].kind, SqlTokenKind::STRING);
    EXPECT_EQ(tokens[1].text, "'Ada''s'");
    EXPECT_EQ(tokens[2].line, 2U);
    EXPECT_EQ(tokens[2].column, 1U);
    EXPECT_EQ(tokens[2].text.data(), query.data() + query.find("FROM"));
    EXPECT_EQ(tokens.back().kind, SqlTokenKind::END);
    EXPECT_EQ(tokens.back().line, 2U);
}

TEST(SqlLexerTest, DecodesEscapedQuotesAndRejectsUnterminatedStrings) {
    const std::string query = "'Ada''s'";
    const std::vector<SqlToken> tokens = SqlLexer::Tokenize(query);
    ASSERT_EQ(tokens[0].kind, SqlTokenKind::STRING);
    EXPECT_EQ(SqlLexer::DecodeString(tokens[0]), "Ada's");

    try {
        static_cast<void>(SqlLexer::Tokenize("'unfinished"));
        FAIL() << "expected an unterminated-string error";
    } catch (const SqlError& error) {
        EXPECT_EQ(error.Code(), SqlErrorCode::SYNTAX_ERROR);
        EXPECT_EQ(error.Line(), 1U);
        EXPECT_EQ(error.Column(), 1U);
    }
}

TEST(SqlLexerTest, RecognizesDecimalAndExponentFloatForms) {
    const std::vector<SqlToken> tokens = SqlLexer::Tokenize("SELECT .5, 1., 1e-2 FROM t");
    EXPECT_EQ(tokens[1].kind, SqlTokenKind::FLOAT);
    EXPECT_EQ(tokens[1].text, ".5");
    EXPECT_EQ(tokens[3].kind, SqlTokenKind::FLOAT);
    EXPECT_EQ(tokens[3].text, "1.");
    EXPECT_EQ(tokens[5].kind, SqlTokenKind::FLOAT);
    EXPECT_EQ(tokens[5].text, "1e-2");
}

TEST(SqlParserTest, ParsesExpressionPrecedenceAndRoundTrips) {
    const std::string query =
        "select a + b * 2 from people where not (a = 1 or b between 2 and 3) "
        "order by a desc limit 4 offset 1";
    const SqlStatement statement = SqlParser(query).Parse();

    ASSERT_EQ(statement.kind, SqlStatementKind::SELECT);
    ASSERT_EQ(statement.projections.size(), 1U);
    ASSERT_EQ(statement.projections[0].expression.kind, SqlExpressionKind::BINARY);
    EXPECT_EQ(statement.projections[0].expression.operation, "+");
    ASSERT_EQ(statement.projections[0].expression.children.size(), 2U);
    EXPECT_EQ(statement.projections[0].expression.children[1].operation, "*");
    EXPECT_EQ(statement.where.kind, SqlExpressionKind::UNARY);
    EXPECT_EQ(statement.limit, 4U);
    EXPECT_EQ(statement.offset, 1U);

    const std::string printed = PrintSql(statement);
    EXPECT_NO_THROW(SqlParser(printed).Parse());
    EXPECT_EQ(PrintSql(SqlParser(printed).Parse()), printed);
}

TEST(SqlParserTest, AppliesBooleanAndArithmeticPrecedence) {
    const SqlStatement statement =
        SqlParser("SELECT NOT a = 1 OR b + 2 * 3 > 4 AND c = 5 FROM t").Parse();
    const SqlExpression& expression = statement.projections[0].expression;

    ASSERT_EQ(expression.kind, SqlExpressionKind::BINARY);
    EXPECT_EQ(expression.operation, "OR");
    ASSERT_EQ(expression.children[0].kind, SqlExpressionKind::UNARY);
    EXPECT_EQ(expression.children[0].children[0].operation, "=");
    ASSERT_EQ(expression.children[1].operation, "AND");
    const SqlExpression& comparison = expression.children[1].children[0];
    ASSERT_EQ(comparison.operation, ">");
    EXPECT_EQ(comparison.children[0].operation, "+");
    EXPECT_EQ(comparison.children[0].children[1].operation, "*");
}

TEST(SqlParserTest, ParsesNegatedPredicatesAndParameters) {
    const std::string sql =
        "SELECT * FROM t WHERE score NOT BETWEEN -2 AND $1 "
        "AND name NOT IN ('a', 'b') OR label LIKE 'row_%'";
    const SqlStatement statement = SqlParser(sql).Parse();
    ASSERT_EQ(statement.where.kind, SqlExpressionKind::BINARY);
    EXPECT_EQ(statement.where.operation, "OR");
    const std::string printed = PrintSql(statement);
    EXPECT_EQ(PrintSql(SqlParser(printed).Parse()), printed);
}

TEST(SqlParserTest, RestrictsAggregateFormsToTheSupportedGrammar) {
    EXPECT_NO_THROW(SqlParser("SELECT count(*) FROM t").Parse());
    const std::vector<std::string> invalid_sql{"SELECT SUM(*) FROM t", "SELECT COUNT(x) FROM t",
                                               "SELECT OTHER(x) FROM t"};
    for (const std::string& sql : invalid_sql) {
        EXPECT_THROW(SqlParser(sql).Parse(), SqlError) << sql;
    }
}

TEST(SqlParserTest, ParsesAndPrintsEverySupportedStatementFamily) {
    const std::vector<std::string> statements{
        "CREATE TABLE IF NOT EXISTS users (id BIGINT PRIMARY KEY, "
        "name VARCHAR(64) NOT NULL UNIQUE, role INT REFERENCES roles(id))",
        "DROP TABLE users",
        "CREATE UNIQUE INDEX users_role ON users(role, id)",
        "INSERT INTO users (id, name) VALUES (1, 'Ada''s'), (2, 'Lin')",
        "SELECT COUNT(*), SUM(id) AS total FROM users u WHERE role IN (1, 2) "
        "GROUP BY role ORDER BY role DESC LIMIT 10 OFFSET 2",
        "UPDATE users SET name = 'Ada', role = role + 1 WHERE id BETWEEN 1 AND 3",
        "DELETE FROM users WHERE name IS NOT NULL",
        "EXPLAIN ANALYZE SELECT * FROM users",
        "SHOW TABLES",
        "DESCRIBE users",
        "CHECKPOINT"};

    for (const std::string& sql : statements) {
        SCOPED_TRACE(sql);
        const std::string printed = PrintSql(SqlParser(sql).Parse());
        EXPECT_EQ(PrintSql(SqlParser(printed).Parse()), printed);
    }
}

TEST(SqlParserTest, ReportsUnexpectedTokenPosition) {
    const std::string query = "SELECT *\nFROM";
    try {
        static_cast<void>(SqlParser(query).Parse());
        FAIL() << "expected a positioned syntax error";
    } catch (const SqlError& error) {
        EXPECT_EQ(error.Code(), SqlErrorCode::SYNTAX_ERROR);
        EXPECT_EQ(error.Line(), 2U);
        EXPECT_EQ(error.Column(), 5U);
    }
}

}  // namespace
}  // namespace relational_db
