#include "sql_executor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "sql_parser.h"

namespace relational_db {

void OperatorStatsSink::Add(OperatorMetrics metrics) {
    const auto found = std::find_if(metrics_.begin(), metrics_.end(), [&metrics](const auto& item) {
        return item.name == metrics.name;
    });
    if (found == metrics_.end()) {
        metrics_.push_back(std::move(metrics));
        return;
    }
    found->rows += metrics.rows;
    found->elapsed_ns += metrics.elapsed_ns;
    found->pages_fetched += metrics.pages_fetched;
    found->pages_missed += metrics.pages_missed;
}

VolcanoOperator::VolcanoOperator(ExecutionContext& context) : context_(context) {}

void VolcanoOperator::Init() {
    if (context_.cancel != nullptr) {
        context_.cancel->Check();
    }
    const BufferPoolStats before = context_.bpm->SnapshotStats();
    const auto start = std::chrono::steady_clock::now();
    InitImpl();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (context_.stats != nullptr) {
        const BufferPoolStats after = context_.bpm->SnapshotStats();
        context_.stats->Add(OperatorMetrics{
            Name(), 0, static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)),
            after.hits + after.misses - before.hits - before.misses, after.misses - before.misses});
    }
}

bool VolcanoOperator::Next(Tuple* tuple, RID* rid) {
    if (tuple == nullptr || rid == nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Operator output pointers cannot be null");
    }
    if (context_.cancel != nullptr) {
        context_.cancel->Check();
    }
    const BufferPoolStats before = context_.bpm->SnapshotStats();
    const auto start = std::chrono::steady_clock::now();
    const bool produced = NextImpl(tuple, rid);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (context_.stats != nullptr) {
        const BufferPoolStats after = context_.bpm->SnapshotStats();
        context_.stats->Add(OperatorMetrics{
            Name(), produced ? 1U : 0U, static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)),
            after.hits + after.misses - before.hits - before.misses, after.misses - before.misses});
    }
    return produced;
}

void VolcanoOperator::Finish() {
    FinishImpl();
}

void VolcanoOperator::FinishImpl() {}

namespace {

enum class BoundKind { LITERAL, COLUMN, UNARY, BINARY, IS_NULL, BETWEEN, IN, AGGREGATE };

struct BoundExpression {
    BoundKind kind{BoundKind::LITERAL};
    std::string operation;
    Value literal;
    size_t column{0};
    size_t aggregate_slot{0};
    std::vector<std::unique_ptr<BoundExpression>> children;

    Value Eval(const Row& row, const std::vector<Value>* aggregates = nullptr) const;
};

struct AggregateDefinition {
    std::string name;
    std::unique_ptr<BoundExpression> input;
    ColumnType result_type{ColumnType::BIGINT};
};

bool IsNullValue(const Value& value) {
    return std::holds_alternative<std::monostate>(value);
}

bool IsNumericValue(const Value& value) {
    return std::holds_alternative<int32_t>(value) || std::holds_alternative<int64_t>(value) ||
           std::holds_alternative<double>(value);
}

long double NumericValue(const Value& value) {
    if (const auto* integer = std::get_if<int32_t>(&value)) {
        return *integer;
    }
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        return static_cast<long double>(*integer);
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return *number;
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "Expected a numeric SQL value");
}

std::optional<int> CompareValues(const Value& left, const Value& right) {
    if (IsNullValue(left) || IsNullValue(right)) {
        return std::nullopt;
    }
    if (IsNumericValue(left) && IsNumericValue(right)) {
        const long double lhs = NumericValue(left);
        const long double rhs = NumericValue(right);
        return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
    }
    if (left.index() != right.index()) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Cannot compare values of different types");
    }
    if (const auto* lhs = std::get_if<std::string>(&left)) {
        const auto& rhs = std::get<std::string>(right);
        return *lhs < rhs ? -1 : (*lhs > rhs ? 1 : 0);
    }
    if (const auto* lhs = std::get_if<bool>(&left)) {
        const bool rhs = std::get<bool>(right);
        return *lhs == rhs ? 0 : (*lhs ? 1 : -1);
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "Unsupported SQL comparison");
}

std::optional<bool> Truth(const Value& value) {
    if (IsNullValue(value)) {
        return std::nullopt;
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean;
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "SQL predicate must have BOOLEAN type");
}

Value BooleanValue(std::optional<bool> value) {
    return value ? Value(*value) : Value(std::monostate{});
}

Value EvaluateLike(const Value& value, const Value& pattern) {
    if (IsNullValue(value) || IsNullValue(pattern)) {
        return std::monostate{};
    }
    const auto* text = std::get_if<std::string>(&value);
    const auto* mask = std::get_if<std::string>(&pattern);
    if (text == nullptr || mask == nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "LIKE operands must be VARCHAR");
    }
    std::vector<uint8_t> previous(text->size() + 1U, 0);
    std::vector<uint8_t> current(text->size() + 1U, 0);
    previous[0] = 1;
    for (char token : *mask) {
        std::fill(current.begin(), current.end(), 0);
        if (token == '%') {
            current[0] = previous[0];
            for (size_t index = 1; index < current.size(); ++index) {
                current[index] = static_cast<uint8_t>(previous[index] || current[index - 1U]);
            }
        } else {
            for (size_t index = 1; index < current.size(); ++index) {
                current[index] = static_cast<uint8_t>(
                    previous[index - 1U] && (token == '_' || token == (*text)[index - 1U]));
            }
        }
        previous.swap(current);
    }
    const bool matches = previous.back() != 0;
    return matches;
}

int64_t CheckedInteger(long double value) {
    if (value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer arithmetic overflow");
    }
    return static_cast<int64_t>(value);
}

int64_t IntegerValue(const Value& value) {
    if (const auto* integer = std::get_if<int32_t>(&value)) {
        return *integer;
    }
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        return *integer;
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "Expected an integer SQL value");
}

int64_t CheckedAdd(int64_t left, int64_t right) {
    if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<int64_t>::min() - right)) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer aggregate overflow");
    }
    return left + right;
}

int64_t CheckedSubtract(int64_t left, int64_t right) {
    if ((right < 0 && left > std::numeric_limits<int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<int64_t>::min() + right)) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer arithmetic overflow");
    }
    return left - right;
}

int64_t CheckedMultiply(int64_t left, int64_t right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    if ((left == -1 && right == std::numeric_limits<int64_t>::min()) ||
        (right == -1 && left == std::numeric_limits<int64_t>::min())) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer arithmetic overflow");
    }
    if (left > 0) {
        if ((right > 0 && left > std::numeric_limits<int64_t>::max() / right) ||
            (right < 0 && right < std::numeric_limits<int64_t>::min() / left)) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer arithmetic overflow");
        }
    } else if ((right > 0 && left < std::numeric_limits<int64_t>::min() / right) ||
               (right < 0 && left < std::numeric_limits<int64_t>::max() / right)) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer arithmetic overflow");
    }
    return left * right;
}

Value EvalBinary(std::string_view operation, const Value& left, const Value& right) {
    if (operation == "AND" || operation == "OR") {
        const std::optional<bool> lhs = Truth(left);
        const std::optional<bool> rhs = Truth(right);
        if (operation == "AND") {
            if ((lhs && !*lhs) || (rhs && !*rhs)) {
                return false;
            }
            return BooleanValue(lhs && rhs ? std::optional<bool>(*lhs && *rhs) : std::nullopt);
        }
        if ((lhs && *lhs) || (rhs && *rhs)) {
            return true;
        }
        return BooleanValue(lhs && rhs ? std::optional<bool>(*lhs || *rhs) : std::nullopt);
    }
    if (operation == "LIKE" || operation == "NOT LIKE") {
        Value matches = EvaluateLike(left, right);
        if (operation == "NOT LIKE" && !IsNullValue(matches)) {
            return !std::get<bool>(matches);
        }
        return matches;
    }
    if (operation == "=" || operation == "!=" || operation == "<>" || operation == "<" ||
        operation == "<=" || operation == ">" || operation == ">=") {
        const std::optional<int> comparison = CompareValues(left, right);
        if (!comparison) {
            return std::monostate{};
        }
        if (operation == "=") {
            return *comparison == 0;
        }
        if (operation == "!=" || operation == "<>") {
            return *comparison != 0;
        }
        if (operation == "<") {
            return *comparison < 0;
        }
        if (operation == "<=") {
            return *comparison <= 0;
        }
        if (operation == ">") {
            return *comparison > 0;
        }
        return *comparison >= 0;
    }
    if (IsNullValue(left) || IsNullValue(right)) {
        return std::monostate{};
    }
    if (!IsNumericValue(left) || !IsNumericValue(right)) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Arithmetic operands must be numeric");
    }
    const bool use_double =
        std::holds_alternative<double>(left) || std::holds_alternative<double>(right);
    const long double lhs = NumericValue(left);
    const long double rhs = NumericValue(right);
    if ((operation == "/" || operation == "%") && rhs == 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Division by zero");
    }
    if (!use_double) {
        const int64_t int_left = IntegerValue(left);
        const int64_t int_right = IntegerValue(right);
        if (operation == "+") {
            return CheckedAdd(int_left, int_right);
        }
        if (operation == "-") {
            return CheckedSubtract(int_left, int_right);
        }
        if (operation == "*") {
            return CheckedMultiply(int_left, int_right);
        }
        if (operation == "/") {
            if (int_left == std::numeric_limits<int64_t>::min() && int_right == -1) {
                throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer arithmetic overflow");
            }
            return int_left / int_right;
        }
        if (operation == "%") {
            if (int_left == std::numeric_limits<int64_t>::min() && int_right == -1) {
                return int64_t{0};
            }
            return int_left % int_right;
        }
    }
    long double result = 0;
    if (operation == "+") {
        result = lhs + rhs;
    } else if (operation == "-") {
        result = lhs - rhs;
    } else if (operation == "*") {
        result = lhs * rhs;
    } else if (operation == "/") {
        return static_cast<double>(lhs / rhs);
    } else if (operation == "%") {
        return CheckedInteger(std::fmod(lhs, rhs));
    } else {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Unsupported binary SQL operator");
    }
    if (use_double) {
        return static_cast<double>(result);
    }
    return CheckedInteger(result);
}

Value BoundExpression::Eval(const Row& row, const std::vector<Value>* aggregates) const {
    if (kind == BoundKind::LITERAL) {
        return literal;
    }
    if (kind == BoundKind::COLUMN) {
        if (column >= row.size()) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Bound column is outside the input row");
        }
        return row[column];
    }
    if (kind == BoundKind::AGGREGATE) {
        if (aggregates == nullptr || aggregate_slot >= aggregates->size()) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Aggregate value is not available");
        }
        return (*aggregates)[aggregate_slot];
    }
    if (kind == BoundKind::UNARY) {
        const Value operand = children.front()->Eval(row, aggregates);
        if (operation == "NOT") {
            const auto truth = Truth(operand);
            return truth ? Value(!*truth) : Value(std::monostate{});
        }
        if (IsNullValue(operand)) {
            return std::monostate{};
        }
        if (!IsNumericValue(operand)) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Unary arithmetic operand must be numeric");
        }
        if (operation == "+") {
            return operand;
        }
        if (!std::holds_alternative<double>(operand)) {
            const int64_t integer = IntegerValue(operand);
            if (integer == std::numeric_limits<int64_t>::min()) {
                throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer arithmetic overflow");
            }
            return -integer;
        }
        const long double result = -NumericValue(operand);
        return static_cast<double>(result);
    }
    if (kind == BoundKind::IS_NULL) {
        const bool is_null = IsNullValue(children.front()->Eval(row, aggregates));
        return operation == "IS NOT" ? !is_null : is_null;
    }
    if (kind == BoundKind::BINARY) {
        return EvalBinary(operation, children[0]->Eval(row, aggregates),
                          children[1]->Eval(row, aggregates));
    }
    if (kind == BoundKind::BETWEEN) {
        const Value value = children[0]->Eval(row, aggregates);
        const Value low = children[1]->Eval(row, aggregates);
        const Value high = children[2]->Eval(row, aggregates);
        const Value result =
            EvalBinary("AND", EvalBinary(">=", value, low), EvalBinary("<=", value, high));
        if (operation == "NOT BETWEEN" && !IsNullValue(result)) {
            return !std::get<bool>(result);
        }
        return result;
    }
    if (kind == BoundKind::IN) {
        const Value value = children.front()->Eval(row, aggregates);
        bool saw_null = false;
        for (size_t index = 1; index < children.size(); ++index) {
            const Value candidate = children[index]->Eval(row, aggregates);
            const Value equal = EvalBinary("=", value, candidate);
            if (const auto* result = std::get_if<bool>(&equal); result != nullptr && *result) {
                return operation == "NOT IN" ? Value(false) : Value(true);
            }
            saw_null = saw_null || IsNullValue(equal);
        }
        if (saw_null) {
            return std::monostate{};
        }
        return operation == "NOT IN" ? Value(true) : Value(false);
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "Unsupported bound expression");
}

std::string Canonical(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

std::string Upper(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return result;
}

Value ParseLiteral(std::string_view literal) {
    if (literal == "NULL" || literal == "null") {
        return std::monostate{};
    }
    if (Canonical(literal) == "true") {
        return true;
    }
    if (Canonical(literal) == "false") {
        return false;
    }
    if (!literal.empty() && literal.front() == '\'') {
        std::string result;
        for (size_t index = 1; index + 1U < literal.size(); ++index) {
            if (literal[index] == '\'' && index + 1U < literal.size() - 1U &&
                literal[index + 1U] == '\'') {
                ++index;
            }
            result.push_back(literal[index]);
        }
        return result;
    }
    if (literal.find_first_of(".eE") != std::string_view::npos) {
        double value = 0;
        const auto parsed = std::from_chars(literal.data(), literal.data() + literal.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != literal.data() + literal.size()) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Invalid DOUBLE literal");
        }
        return value;
    }
    int64_t value = 0;
    const auto parsed = std::from_chars(literal.data(), literal.data() + literal.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != literal.data() + literal.size()) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Invalid integer literal");
    }
    return value;
}

size_t ResolveColumn(const SqlExpression& expression, const Schema& schema,
                     std::string_view table_name, std::string_view alias) {
    if (expression.kind != SqlExpressionKind::COLUMN) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Expected a column reference");
    }
    if (!expression.qualifier.empty() && Canonical(expression.qualifier) != Canonical(table_name) &&
        Canonical(expression.qualifier) != Canonical(alias)) {
        throw DbError(ErrorCode::UNKNOWN_TABLE, "Unknown table qualifier");
    }
    const std::string name = Canonical(expression.name);
    for (size_t index = 0; index < schema.columns.size(); ++index) {
        if (Canonical(schema.columns[index].name) == name) {
            return index;
        }
    }
    throw DbError(ErrorCode::UNKNOWN_COLUMN, "Unknown column: " + std::string(expression.name));
}

using AggregateList = std::vector<AggregateDefinition>;

std::optional<Value> AstLiteral(const SqlExpression& expression);

std::unique_ptr<BoundExpression> CompileExpression(const SqlExpression& expression,
                                                   const Schema& schema,
                                                   std::string_view table_name,
                                                   std::string_view alias,
                                                   AggregateList* aggregates = nullptr) {
    auto result = std::make_unique<BoundExpression>();
    switch (expression.kind) {
        case SqlExpressionKind::LITERAL:
            result->kind = BoundKind::LITERAL;
            result->literal = ParseLiteral(expression.value);
            break;
        case SqlExpressionKind::COLUMN:
            result->kind = BoundKind::COLUMN;
            result->column = ResolveColumn(expression, schema, table_name, alias);
            break;
        case SqlExpressionKind::UNARY:
            if (const std::optional<Value> literal = AstLiteral(expression)) {
                result->kind = BoundKind::LITERAL;
                result->literal = *literal;
                break;
            }
            result->kind = BoundKind::UNARY;
            result->operation = Upper(expression.operation);
            result->children.push_back(CompileExpression(expression.children.front(), schema,
                                                         table_name, alias, aggregates));
            break;
        case SqlExpressionKind::BINARY:
            result->kind = BoundKind::BINARY;
            result->operation = Upper(expression.operation);
            for (const SqlExpression& child : expression.children) {
                result->children.push_back(
                    CompileExpression(child, schema, table_name, alias, aggregates));
            }
            break;
        case SqlExpressionKind::IS_NULL:
            result->kind = BoundKind::IS_NULL;
            result->operation = Upper(expression.operation);
            result->children.push_back(CompileExpression(expression.children.front(), schema,
                                                         table_name, alias, aggregates));
            break;
        case SqlExpressionKind::BETWEEN:
            result->kind = BoundKind::BETWEEN;
            result->operation = Upper(expression.operation);
            for (const SqlExpression& child : expression.children) {
                result->children.push_back(
                    CompileExpression(child, schema, table_name, alias, aggregates));
            }
            break;
        case SqlExpressionKind::IN:
            result->kind = BoundKind::IN;
            result->operation = Upper(expression.operation);
            for (const SqlExpression& child : expression.children) {
                result->children.push_back(
                    CompileExpression(child, schema, table_name, alias, aggregates));
            }
            break;
        case SqlExpressionKind::FUNCTION: {
            const std::string name = Canonical(expression.name);
            if (aggregates == nullptr || (name != "count" && name != "sum" && name != "min" &&
                                          name != "max" && name != "avg")) {
                throw DbError(ErrorCode::INVALID_ARGUMENT, "Unsupported SQL function");
            }
            AggregateDefinition definition;
            definition.name = name;
            if (name != "count" || expression.children.front().kind != SqlExpressionKind::STAR) {
                definition.input = CompileExpression(expression.children.front(), schema,
                                                     table_name, alias, nullptr);
                if (definition.input->kind == BoundKind::COLUMN) {
                    definition.result_type = schema.columns[definition.input->column].type;
                }
            }
            if (name == "count") {
                definition.result_type = ColumnType::BIGINT;
            } else if (name == "avg") {
                definition.result_type = ColumnType::DOUBLE;
            } else if (name == "sum" && definition.result_type != ColumnType::DOUBLE) {
                definition.result_type = ColumnType::BIGINT;
            }
            result->kind = BoundKind::AGGREGATE;
            result->aggregate_slot = aggregates->size();
            result->operation = name;
            aggregates->push_back(std::move(definition));
            break;
        }
        case SqlExpressionKind::EMPTY:
        case SqlExpressionKind::PARAMETER:
        case SqlExpressionKind::STAR:
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Expression is not supported here");
    }
    return result;
}

ColumnType ExpressionType(const SqlExpression& expression, const Schema& schema,
                          std::string_view table, std::string_view alias,
                          const AggregateList& aggregates) {
    if (expression.kind == SqlExpressionKind::FUNCTION) {
        const std::string name = Canonical(expression.name);
        if (name == "count" || name == "avg") {
            return name == "count" ? ColumnType::BIGINT : ColumnType::DOUBLE;
        }
        if (name == "sum") {
            if (expression.children.front().kind == SqlExpressionKind::COLUMN) {
                const ColumnType input =
                    schema.columns[ResolveColumn(expression.children.front(), schema, table, alias)]
                        .type;
                return input == ColumnType::DOUBLE ? ColumnType::DOUBLE : ColumnType::BIGINT;
            }
            return ColumnType::BIGINT;
        }
        if (expression.children.front().kind == SqlExpressionKind::COLUMN) {
            return schema.columns[ResolveColumn(expression.children.front(), schema, table, alias)]
                .type;
        }
    }
    if (expression.kind == SqlExpressionKind::COLUMN) {
        return schema.columns[ResolveColumn(expression, schema, table, alias)].type;
    }
    if (expression.kind == SqlExpressionKind::LITERAL) {
        const Value value = ParseLiteral(expression.value);
        if (std::holds_alternative<double>(value)) {
            return ColumnType::DOUBLE;
        }
        if (std::holds_alternative<bool>(value)) {
            return ColumnType::BOOLEAN;
        }
        if (std::holds_alternative<std::string>(value)) {
            return ColumnType::VARCHAR;
        }
        return ColumnType::BIGINT;
    }
    if (expression.kind == SqlExpressionKind::BINARY &&
        (expression.operation == "=" || expression.operation == "!=" ||
         expression.operation == "<>" || expression.operation == "<" ||
         expression.operation == "<=" || expression.operation == ">" ||
         expression.operation == ">=" || Canonical(expression.operation) == "and" ||
         Canonical(expression.operation) == "or" || Canonical(expression.operation) == "like")) {
        return ColumnType::BOOLEAN;
    }
    if (expression.kind == SqlExpressionKind::IS_NULL ||
        expression.kind == SqlExpressionKind::BETWEEN || expression.kind == SqlExpressionKind::IN) {
        return ColumnType::BOOLEAN;
    }
    if (expression.kind == SqlExpressionKind::UNARY && Canonical(expression.operation) == "not") {
        return ColumnType::BOOLEAN;
    }
    if (expression.kind == SqlExpressionKind::BINARY &&
        (expression.operation == "/" ||
         (expression.children[0].kind == SqlExpressionKind::LITERAL &&
          expression.children[0].value.find_first_of(".eE") != std::string_view::npos) ||
         (expression.children[1].kind == SqlExpressionKind::LITERAL &&
          expression.children[1].value.find_first_of(".eE") != std::string_view::npos))) {
        return ColumnType::DOUBLE;
    }
    static_cast<void>(aggregates);
    return ColumnType::BIGINT;
}

Value CoerceValue(Value value, const Column& column) {
    if (IsNullValue(value)) {
        return value;
    }
    switch (column.type) {
        case ColumnType::INT:
            if (IsNumericValue(value)) {
                const long double numeric = NumericValue(value);
                if (std::trunc(numeric) != numeric ||
                    numeric < std::numeric_limits<int32_t>::min() ||
                    numeric > std::numeric_limits<int32_t>::max()) {
                    throw DbError(ErrorCode::INVALID_ARGUMENT, "Value is outside INT range");
                }
                return static_cast<int32_t>(numeric);
            }
            break;
        case ColumnType::BIGINT:
            if (IsNumericValue(value)) {
                if (const auto* integer = std::get_if<int64_t>(&value)) {
                    return *integer;
                }
                const long double numeric = NumericValue(value);
                const long double bigint_limit = std::ldexp(1.0L, 63);
                if (std::trunc(numeric) != numeric || numeric < -bigint_limit ||
                    numeric >= bigint_limit) {
                    throw DbError(ErrorCode::INVALID_ARGUMENT, "Value is outside BIGINT range");
                }
                return static_cast<int64_t>(numeric);
            }
            break;
        case ColumnType::DOUBLE:
            if (IsNumericValue(value)) {
                return static_cast<double>(NumericValue(value));
            }
            break;
        case ColumnType::BOOLEAN:
            if (std::holds_alternative<bool>(value)) {
                return value;
            }
            break;
        case ColumnType::VARCHAR:
            if (std::holds_alternative<std::string>(value)) {
                return value;
            }
            break;
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "Value type does not match column " + column.name);
}

Schema ProjectionSchema(const std::vector<SqlSelectItem>& items, const Schema& input,
                        std::string_view table, std::string_view alias,
                        const AggregateList& aggregates) {
    Schema output;
    for (size_t index = 0; index < items.size(); ++index) {
        const SqlSelectItem& item = items[index];
        const ColumnType type = ExpressionType(item.expression, input, table, alias, aggregates);
        output.columns.push_back(Column{"__projection_" + std::to_string(index), type,
                                        type == ColumnType::VARCHAR ? uint16_t{255} : uint16_t{0}});
    }
    output.Validate();
    return output;
}

std::string ProjectionName(const SqlSelectItem& item, size_t index) {
    if (!item.alias.empty()) {
        return std::string(item.alias);
    }
    if (item.expression.kind == SqlExpressionKind::COLUMN) {
        return std::string(item.expression.name);
    }
    if (item.expression.kind == SqlExpressionKind::FUNCTION) {
        const std::string function = Upper(item.expression.name);
        const std::string argument =
            item.expression.children.front().kind == SqlExpressionKind::STAR
                ? "*"
                : std::string(item.expression.children.front().name);
        return function + "(" + argument + ")";
    }
    return "column" + std::to_string(index + 1U);
}

std::string FormatSqlValue(const Value& value) {
    if (IsNullValue(value)) {
        return "NULL";
    }
    if (const auto* integer = std::get_if<int32_t>(&value)) {
        return std::to_string(*integer);
    }
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        return std::to_string(*integer);
    }
    if (const auto* number = std::get_if<double>(&value)) {
        std::ostringstream stream;
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << *number;
        return stream.str();
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        std::string escaped = "'";
        for (char ch : *text) {
            escaped.push_back(ch);
            if (ch == '\'') {
                escaped.push_back('\'');
            }
        }
        escaped.push_back('\'');
        return escaped;
    }
    return std::get<bool>(value) ? "TRUE" : "FALSE";
}

struct BufferedRow {
    Tuple tuple;
    RID rid;
    std::vector<Value> keys;
};

class TableScanOperator : public VolcanoOperator {
   public:
    TableScanOperator(ExecutionContext& context, CatalogTable& table)
        : VolcanoOperator(context), table_(table) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + " (" + table_.name + ")";
    }

   protected:
    CatalogTable& table_;
};

class SeqScan final : public TableScanOperator {
   public:
    using TableScanOperator::TableScanOperator;

   protected:
    void InitImpl() override {
        iterator_ = table_.heap->Begin();
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        if (context_.cancel != nullptr) {
            context_.cancel->Check();
        }
        return iterator_.Next(*rid, *tuple);
    }

    std::string Name() const override {
        return "SeqScan";
    }

   private:
    TableIterator iterator_;
};

class IndexRangeScan : public TableScanOperator {
   public:
    IndexRangeScan(ExecutionContext& context, CatalogTable& table, CatalogIndex& index,
                   const IndexKey16& low, const IndexKey16& high, bool include_low,
                   bool include_high)
        : TableScanOperator(context, table),
          index_(index),
          low_(low),
          high_(high),
          include_low_(include_low),
          include_high_(include_high) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + " (" + index_.name + ")";
    }

   protected:
    void InitImpl() override {
        iterator_ = index_.tree->LowerBound(low_);
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        while (std::optional<std::pair<IndexKey16, uint64_t>> entry = iterator_.Next()) {
            if (context_.cancel != nullptr) {
                context_.cancel->Check();
            }
            const IndexKey16Less less;
            if (less(high_, entry->first) || (!include_high_ && entry->first == high_)) {
                FinishImpl();
                return false;
            }
            if (!include_low_ && entry->first == low_) {
                continue;
            }
            *rid = RID::Unpack(entry->second);
            const std::optional<Tuple> found = table_.heap->Get(*rid);
            if (!found) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Index entry references a missing row",
                              rid->page_id);
            }
            *tuple = *found;
            return true;
        }
        return false;
    }

    std::string Name() const override {
        return "IndexRangeScan";
    }

    void FinishImpl() override {
        iterator_ = index_.tree->End();
    }

   private:
    CatalogIndex& index_;
    IndexKey16 low_;
    IndexKey16 high_;
    bool include_low_;
    bool include_high_;
    SqlIndexTree::Iterator iterator_;
};

class IndexPointLookup final : public IndexRangeScan {
   public:
    IndexPointLookup(ExecutionContext& context, CatalogTable& table, CatalogIndex& index,
                     const Value& value)
        : IndexRangeScan(context, table, index, Low(value), High(value), true, true) {}

   protected:
    std::string Name() const override {
        return "IndexPointLookup";
    }

   private:
    static IndexKey16 Low(const Value& value) {
        IndexKey16 key = IndexPrefix(value);
        std::fill(key.bytes.begin() + 8, key.bytes.end(), 0);
        return key;
    }

    static IndexKey16 High(const Value& value) {
        IndexKey16 key = IndexPrefix(value);
        std::fill(key.bytes.begin() + 8, key.bytes.end(), UINT8_MAX);
        return key;
    }
};

class IndexOrderScan final : public TableScanOperator {
   public:
    IndexOrderScan(ExecutionContext& context, CatalogTable& table, CatalogIndex& index,
                   bool descending)
        : TableScanOperator(context, table), index_(index), descending_(descending) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + " (" + index_.name +
               (descending_ ? ", DESC)" : ", ASC)");
    }

   protected:
    void InitImpl() override {
        if (descending_) {
            packed_rids_.clear();
            position_ = 0;
            SqlIndexTree::Iterator iterator = index_.tree->Begin();
            while (std::optional<std::pair<IndexKey16, uint64_t>> entry = iterator.Next()) {
                if (context_.cancel != nullptr) {
                    context_.cancel->Check();
                }
                packed_rids_.push_back(entry->second);
            }
            std::reverse(packed_rids_.begin(), packed_rids_.end());
        } else {
            iterator_ = index_.tree->Begin();
        }
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        if (descending_) {
            if (position_ >= packed_rids_.size()) {
                return false;
            }
            *rid = RID::Unpack(packed_rids_[position_++]);
        } else {
            const auto entry = iterator_.Next();
            if (!entry) {
                return false;
            }
            *rid = RID::Unpack(entry->second);
        }
        const std::optional<Tuple> found = table_.heap->Get(*rid);
        if (!found) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Index entry references a missing row",
                          rid->page_id);
        }
        *tuple = *found;
        return true;
    }

    void FinishImpl() override {
        iterator_ = index_.tree->End();
        packed_rids_.clear();
    }

    std::string Name() const override {
        return "IndexRangeScan";
    }

   private:
    CatalogIndex& index_;
    bool descending_;
    SqlIndexTree::Iterator iterator_;
    std::vector<uint64_t> packed_rids_;
    size_t position_{0};
};

class Filter final : public VolcanoOperator {
   public:
    Filter(ExecutionContext& context, std::unique_ptr<VolcanoOperator> child, Schema schema,
           std::unique_ptr<BoundExpression> predicate)
        : VolcanoOperator(context),
          child_(std::move(child)),
          schema_(std::move(schema)),
          predicate_(std::move(predicate)) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + "\n" + child_->Explain(depth + 1U);
    }

   protected:
    void InitImpl() override {
        child_->Init();
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        while (child_->Next(tuple, rid)) {
            const Row row = DeserializeTuple(schema_, *tuple);
            const Value result = predicate_->Eval(row);
            const auto truth = Truth(result);
            if (truth && *truth) {
                return true;
            }
        }
        return false;
    }

    void FinishImpl() override {
        child_->Finish();
    }

    std::string Name() const override {
        return "Filter";
    }

   private:
    std::unique_ptr<VolcanoOperator> child_;
    Schema schema_;
    std::unique_ptr<BoundExpression> predicate_;
};

class Projection final : public VolcanoOperator {
   public:
    Projection(ExecutionContext& context, std::unique_ptr<VolcanoOperator> child, Schema input,
               Schema output, std::vector<std::unique_ptr<BoundExpression>> expressions)
        : VolcanoOperator(context),
          child_(std::move(child)),
          input_(std::move(input)),
          output_(std::move(output)),
          expressions_(std::move(expressions)) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + "\n" + child_->Explain(depth + 1U);
    }

   protected:
    void InitImpl() override {
        child_->Init();
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        Tuple input_tuple;
        if (!child_->Next(&input_tuple, rid)) {
            return false;
        }
        const Row source = DeserializeTuple(input_, input_tuple);
        Row projected;
        projected.reserve(expressions_.size());
        for (size_t index = 0; index < expressions_.size(); ++index) {
            projected.push_back(
                CoerceValue(expressions_[index]->Eval(source), output_.columns[index]));
        }
        *tuple = SerializeTuple(output_, projected);
        return true;
    }

    void FinishImpl() override {
        child_->Finish();
    }

    std::string Name() const override {
        return "Projection";
    }

   private:
    std::unique_ptr<VolcanoOperator> child_;
    Schema input_;
    Schema output_;
    std::vector<std::unique_ptr<BoundExpression>> expressions_;
};

class Sort final : public VolcanoOperator {
   public:
    Sort(ExecutionContext& context, std::unique_ptr<VolcanoOperator> child, Schema schema,
         std::vector<std::unique_ptr<BoundExpression>> keys, std::vector<bool> descending)
        : VolcanoOperator(context),
          child_(std::move(child)),
          schema_(std::move(schema)),
          keys_(std::move(keys)),
          descending_(std::move(descending)) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + "\n" + child_->Explain(depth + 1U);
    }

   protected:
    void InitImpl() override {
        child_->Init();
        Tuple tuple;
        RID rid{};
        while (child_->Next(&tuple, &rid)) {
            if (rows_.size() >= MAX_SORT_ROWS) {
                throw DbError(ErrorCode::SORT_LIMIT_EXCEEDED, "Sort exceeds MAX_SORT_ROWS");
            }
            const Row row = DeserializeTuple(schema_, tuple);
            BufferedRow buffered{std::move(tuple), rid, {}};
            buffered.keys.reserve(keys_.size());
            for (const auto& key : keys_) {
                buffered.keys.push_back(key->Eval(row));
            }
            rows_.push_back(std::move(buffered));
        }
        std::stable_sort(
            rows_.begin(), rows_.end(), [this](const BufferedRow& left, const BufferedRow& right) {
                for (size_t index = 0; index < keys_.size(); ++index) {
                    if (IsNullValue(left.keys[index]) || IsNullValue(right.keys[index])) {
                        if (IsNullValue(left.keys[index]) != IsNullValue(right.keys[index])) {
                            return !IsNullValue(left.keys[index]);
                        }
                        continue;
                    }
                    const int comparison =
                        CompareValues(left.keys[index], right.keys[index]).value();
                    if (comparison != 0) {
                        return descending_[index] ? comparison > 0 : comparison < 0;
                    }
                }
                return false;
            });
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        if (position_ >= rows_.size()) {
            return false;
        }
        *tuple = rows_[position_].tuple;
        *rid = rows_[position_].rid;
        ++position_;
        return true;
    }

    void FinishImpl() override {
        child_->Finish();
    }

    std::string Name() const override {
        return "Sort";
    }

   private:
    std::unique_ptr<VolcanoOperator> child_;
    Schema schema_;
    std::vector<std::unique_ptr<BoundExpression>> keys_;
    std::vector<bool> descending_;
    std::vector<BufferedRow> rows_;
    size_t position_{0};
};

class LimitOffset final : public VolcanoOperator {
   public:
    LimitOffset(ExecutionContext& context, std::unique_ptr<VolcanoOperator> child, uint64_t limit,
                uint64_t offset, bool has_limit)
        : VolcanoOperator(context),
          child_(std::move(child)),
          limit_(limit),
          offset_(offset),
          has_limit_(has_limit) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() +
               " (limit=" + (has_limit_ ? std::to_string(limit_) : "ALL") +
               ", offset=" + std::to_string(offset_) + ")\n" + child_->Explain(depth + 1U);
    }

   protected:
    void InitImpl() override {
        child_->Init();
        emitted_ = 0;
        skipped_ = 0;
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        while (skipped_ < offset_) {
            Tuple ignored;
            RID ignored_rid{};
            if (!child_->Next(&ignored, &ignored_rid)) {
                return false;
            }
            ++skipped_;
        }
        if (has_limit_ && emitted_ >= limit_) {
            return false;
        }
        if (!child_->Next(tuple, rid)) {
            return false;
        }
        ++emitted_;
        return true;
    }

    void FinishImpl() override {
        child_->Finish();
    }

    std::string Name() const override {
        return "LimitOffset";
    }

   private:
    std::unique_ptr<VolcanoOperator> child_;
    uint64_t limit_;
    uint64_t offset_;
    bool has_limit_;
    uint64_t emitted_{0};
    uint64_t skipped_{0};
};

struct AggregateState {
    uint64_t count{0};
    Value value;
};

struct GroupState {
    Row representative;
    std::vector<AggregateState> aggregates;
};

std::string GroupKey(const Row& values) {
    std::string key;
    for (const Value& value : values) {
        const std::string formatted = FormatSqlValue(value);
        key.push_back(static_cast<char>(value.index()));
        const uint64_t length = formatted.size();
        for (size_t byte = 0; byte < sizeof(length); ++byte) {
            key.push_back(static_cast<char>(length >> (byte * 8U)));
        }
        key.append(formatted);
    }
    return key;
}

class Aggregate final : public VolcanoOperator {
   public:
    Aggregate(ExecutionContext& context, std::unique_ptr<VolcanoOperator> child, Schema input,
              Schema output, std::vector<std::unique_ptr<BoundExpression>> projection,
              std::vector<std::unique_ptr<BoundExpression>> group_by, AggregateList definitions)
        : VolcanoOperator(context),
          child_(std::move(child)),
          input_(std::move(input)),
          output_(std::move(output)),
          projection_(std::move(projection)),
          group_by_(std::move(group_by)),
          definitions_(std::move(definitions)) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() +
               (group_by_.empty() ? " (global)" : " (hash group by)") + "\n" +
               child_->Explain(depth + 1U);
    }

   protected:
    void InitImpl() override {
        child_->Init();
        std::unordered_map<std::string, size_t> group_indexes;
        std::vector<GroupState> groups;
        Tuple tuple;
        RID rid{};
        while (child_->Next(&tuple, &rid)) {
            if (context_.cancel != nullptr) {
                context_.cancel->Check();
            }
            const Row row = DeserializeTuple(input_, tuple);
            Row group_values;
            group_values.reserve(group_by_.size());
            for (const auto& expression : group_by_) {
                group_values.push_back(expression->Eval(row));
            }
            const std::string key = GroupKey(group_values);
            auto [position, inserted] = group_indexes.emplace(key, groups.size());
            if (inserted) {
                if (groups.size() >= MAX_RESULT_ROWS) {
                    throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED,
                                  "Aggregate exceeds MAX_RESULT_ROWS groups");
                }
                groups.emplace_back();
                GroupState& group = groups.back();
                group.representative = row;
                group.aggregates.resize(definitions_.size());
            }
            GroupState& group = groups[position->second];
            for (size_t index = 0; index < definitions_.size(); ++index) {
                const AggregateDefinition& definition = definitions_[index];
                AggregateState& state = group.aggregates[index];
                const Value value =
                    definition.input ? definition.input->Eval(row) : Value(int64_t{1});
                if (definition.name == "count") {
                    if (!definition.input || !IsNullValue(value)) {
                        ++state.count;
                    }
                    state.value = static_cast<int64_t>(state.count);
                    continue;
                }
                if (IsNullValue(value)) {
                    continue;
                }
                if (definition.name == "sum" && definition.result_type != ColumnType::DOUBLE) {
                    const int64_t previous = state.count == 0 ? 0 : IntegerValue(state.value);
                    state.value = CheckedAdd(previous, IntegerValue(value));
                    ++state.count;
                } else if (definition.name == "sum" || definition.name == "avg") {
                    const double number = static_cast<double>(NumericValue(value));
                    const double previous =
                        state.count == 0 ? 0.0 : static_cast<double>(NumericValue(state.value));
                    state.value = previous + number;
                    ++state.count;
                } else if (state.count == 0) {
                    state.value = value;
                    state.count = 1;
                } else {
                    const int order = CompareValues(value, state.value).value();
                    if ((definition.name == "min" && order < 0) ||
                        (definition.name == "max" && order > 0)) {
                        state.value = value;
                    }
                    ++state.count;
                }
            }
        }

        if (group_by_.empty() && groups.empty()) {
            GroupState empty;
            empty.aggregates.resize(definitions_.size());
            groups.push_back(std::move(empty));
        }
        rows_.reserve(groups.size());
        for (GroupState& group : groups) {
            std::vector<Value> values;
            values.reserve(definitions_.size());
            for (size_t index = 0; index < definitions_.size(); ++index) {
                const AggregateDefinition& definition = definitions_[index];
                const AggregateState& state = group.aggregates[index];
                Value value = state.value;
                if (definition.name == "count") {
                    value = static_cast<int64_t>(state.count);
                } else if (state.count == 0) {
                    value = std::monostate{};
                } else if (definition.name == "avg") {
                    value = static_cast<double>(NumericValue(state.value) /
                                                static_cast<long double>(state.count));
                }
                values.push_back(std::move(value));
            }
            Row output_row;
            output_row.reserve(projection_.size());
            for (size_t index = 0; index < projection_.size(); ++index) {
                output_row.push_back(
                    CoerceValue(projection_[index]->Eval(group.representative, &values),
                                output_.columns[index]));
            }
            rows_.push_back(SerializeTuple(output_, output_row));
        }
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        if (position_ >= rows_.size()) {
            return false;
        }
        *tuple = rows_[position_++];
        *rid = RID{};
        return true;
    }

    std::string Name() const override {
        return "Aggregate";
    }

   private:
    std::unique_ptr<VolcanoOperator> child_;
    Schema input_;
    Schema output_;
    std::vector<std::unique_ptr<BoundExpression>> projection_;
    std::vector<std::unique_ptr<BoundExpression>> group_by_;
    AggregateList definitions_;
    std::vector<Tuple> rows_;
    size_t position_{0};
};

struct IndexConstraint {
    size_t column{0};
    std::string operation;
    Value value;
    std::optional<Value> between_high;
};

std::optional<Value> AstLiteral(const SqlExpression& expression) {
    if (expression.kind == SqlExpressionKind::LITERAL) {
        return ParseLiteral(expression.value);
    }
    if (expression.kind == SqlExpressionKind::UNARY && expression.children.size() == 1U &&
        (expression.operation == "-" || expression.operation == "+")) {
        const SqlExpression& child = expression.children.front();
        if (expression.operation == "-" && child.kind == SqlExpressionKind::LITERAL &&
            child.value.find_first_of(".eE") == std::string_view::npos) {
            uint64_t magnitude = 0;
            const auto parsed = std::from_chars(child.value.data(),
                                                child.value.data() + child.value.size(), magnitude);
            const uint64_t minimum_magnitude =
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U;
            if (parsed.ec != std::errc{} || parsed.ptr != child.value.data() + child.value.size() ||
                magnitude > minimum_magnitude) {
                throw DbError(ErrorCode::INVALID_ARGUMENT, "Integer literal is out of range");
            }
            if (magnitude == minimum_magnitude) {
                return std::numeric_limits<int64_t>::min();
            }
            return -static_cast<int64_t>(magnitude);
        }
        const std::optional<Value> operand = AstLiteral(expression.children.front());
        if (!operand || !IsNumericValue(*operand)) {
            return std::nullopt;
        }
        if (expression.operation == "+") {
            return *operand;
        }
        if (const auto* number = std::get_if<double>(&*operand)) {
            return -*number;
        }
        return -CheckedInteger(NumericValue(*operand));
    }
    return std::nullopt;
}

void CollectIndexConstraints(const SqlExpression& where, const Schema& schema,
                             std::string_view table, std::string_view alias,
                             std::vector<IndexConstraint>& result) {
    if (where.kind == SqlExpressionKind::BINARY && Canonical(where.operation) == "and") {
        for (const SqlExpression& child : where.children) {
            CollectIndexConstraints(child, schema, table, alias, result);
        }
        return;
    }
    if (where.kind == SqlExpressionKind::BETWEEN && where.children.size() == 3U) {
        const std::optional<Value> low = AstLiteral(where.children[1]);
        const std::optional<Value> high = AstLiteral(where.children[2]);
        if (where.children[0].kind == SqlExpressionKind::COLUMN && low && high) {
            result.push_back(
                {ResolveColumn(where.children[0], schema, table, alias), "BETWEEN", *low, *high});
        }
        return;
    }
    if (where.kind != SqlExpressionKind::BINARY || where.children.size() != 2U) {
        return;
    }
    const SqlExpression& left = where.children[0];
    const SqlExpression& right = where.children[1];
    const std::optional<Value> right_value = AstLiteral(right);
    const std::optional<Value> left_value = AstLiteral(left);
    if (left.kind == SqlExpressionKind::COLUMN && right_value) {
        result.push_back({ResolveColumn(left, schema, table, alias), Upper(where.operation),
                          *right_value, std::nullopt});
        return;
    }
    if (right.kind == SqlExpressionKind::COLUMN && left_value) {
        std::string operation = Upper(where.operation);
        if (operation == "<") {
            operation = ">";
        } else if (operation == "<=") {
            operation = ">=";
        } else if (operation == ">") {
            operation = "<";
        } else if (operation == ">=") {
            operation = "<=";
        }
        result.push_back({ResolveColumn(right, schema, table, alias), std::move(operation),
                          *left_value, std::nullopt});
    }
}

bool IsOrderableIndex(const CatalogIndex& index, const Schema& schema) {
    return schema.columns[index.column].type != ColumnType::VARCHAR;
}

IndexKey16 PrefixBoundary(const Value& value, uint8_t suffix) {
    IndexKey16 key = IndexPrefix(value);
    std::fill(key.bytes.begin() + 8, key.bytes.end(), suffix);
    return key;
}

bool IsAggregateQuery(const std::vector<SqlSelectItem>& items,
                      const std::vector<SqlExpression>& group_by) {
    return !group_by.empty() ||
           std::any_of(items.begin(), items.end(), [](const SqlSelectItem& item) {
               return item.expression.kind == SqlExpressionKind::FUNCTION;
           });
}

bool SameExpression(const SqlExpression& left, const SqlExpression& right) {
    if (left.kind != right.kind || left.children.size() != right.children.size()) {
        return false;
    }
    if (left.kind == SqlExpressionKind::COLUMN) {
        if (Canonical(left.name) != Canonical(right.name) ||
            Canonical(left.qualifier) != Canonical(right.qualifier)) {
            return false;
        }
    } else if (left.kind == SqlExpressionKind::FUNCTION) {
        if (Canonical(left.name) != Canonical(right.name)) {
            return false;
        }
    } else if (left.kind == SqlExpressionKind::LITERAL) {
        if (left.value != right.value) {
            return false;
        }
    } else if (Upper(left.operation) != Upper(right.operation)) {
        return false;
    }
    for (size_t index = 0; index < left.children.size(); ++index) {
        if (!SameExpression(left.children[index], right.children[index])) {
            return false;
        }
    }
    return true;
}

std::optional<size_t> OutputColumnForOrder(const SqlExpression& order,
                                           const std::vector<SqlSelectItem>& projections) {
    for (size_t index = 0; index < projections.size(); ++index) {
        const SqlSelectItem& projection = projections[index];
        if (order.kind == SqlExpressionKind::COLUMN && !projection.alias.empty() &&
            Canonical(order.name) == Canonical(projection.alias)) {
            return index;
        }
        if (SameExpression(order, projection.expression)) {
            return index;
        }
    }
    return std::nullopt;
}

void ExpandStar(std::vector<SqlSelectItem>& items, const Schema& schema) {
    std::vector<SqlSelectItem> expanded;
    for (const SqlSelectItem& item : items) {
        if (!item.star) {
            expanded.push_back(item);
            continue;
        }
        for (const Column& column : schema.columns) {
            SqlSelectItem projection;
            projection.expression.kind = SqlExpressionKind::COLUMN;
            projection.expression.name = column.name;
            projection.alias = {};
            expanded.push_back(std::move(projection));
        }
    }
    items = std::move(expanded);
}

struct QueryPlan {
    std::unique_ptr<VolcanoOperator> root;
    Schema output_schema;
    std::vector<std::string> output_names;
    std::string access_path;
};

std::unique_ptr<VolcanoOperator> BuildScan(ExecutionContext& context, CatalogTable& table,
                                           const SqlStatement& statement, std::string_view alias,
                                           std::string& access_path) {
    std::vector<IndexConstraint> constraints;
    if (statement.where.kind != SqlExpressionKind::EMPTY) {
        CollectIndexConstraints(statement.where, table.schema, table.name, alias, constraints);
    }
    CatalogIndex* chosen_index = nullptr;
    std::vector<IndexConstraint> chosen_constraints;
    for (CatalogIndex& index : table.indexes) {
        std::vector<IndexConstraint> matching;
        for (const IndexConstraint& constraint : constraints) {
            if (constraint.column == index.column) {
                matching.push_back(constraint);
            }
        }
        const auto equality =
            std::find_if(matching.begin(), matching.end(), [](const IndexConstraint& item) {
                return item.operation == "=" && !IsNullValue(item.value);
            });
        const bool range =
            std::any_of(matching.begin(), matching.end(), [](const IndexConstraint& item) {
                return item.operation == "BETWEEN" || item.operation == "<" ||
                       item.operation == "<=" || item.operation == ">" || item.operation == ">=";
            });
        if (equality != matching.end() || range) {
            chosen_index = &index;
            chosen_constraints = std::move(matching);
            break;
        }
    }

    if (chosen_index != nullptr) {
        const auto equality =
            std::find_if(chosen_constraints.begin(), chosen_constraints.end(),
                         [](const IndexConstraint& item) { return item.operation == "="; });
        if (equality != chosen_constraints.end()) {
            access_path = "index " + chosen_index->name;
            return std::make_unique<IndexPointLookup>(
                context, table, *chosen_index,
                CoerceValue(equality->value, table.schema.columns[chosen_index->column]));
        }
        if (IsOrderableIndex(*chosen_index, table.schema)) {
            std::optional<std::pair<Value, bool>> lower;
            std::optional<std::pair<Value, bool>> upper;
            for (const IndexConstraint& constraint : chosen_constraints) {
                if (constraint.operation == "BETWEEN") {
                    lower = std::make_pair(
                        CoerceValue(constraint.value, table.schema.columns[chosen_index->column]),
                        true);
                    upper = std::make_pair(CoerceValue(*constraint.between_high,
                                                       table.schema.columns[chosen_index->column]),
                                           true);
                } else if (constraint.operation == ">" || constraint.operation == ">=") {
                    const Value value =
                        CoerceValue(constraint.value, table.schema.columns[chosen_index->column]);
                    if (!lower || CompareValues(lower->first, value).value() < 0) {
                        lower = std::make_pair(value, constraint.operation == ">=");
                    }
                } else if (constraint.operation == "<" || constraint.operation == "<=") {
                    const Value value =
                        CoerceValue(constraint.value, table.schema.columns[chosen_index->column]);
                    if (!upper || CompareValues(upper->first, value).value() > 0) {
                        upper = std::make_pair(value, constraint.operation == "<=");
                    }
                }
            }
            IndexKey16 low{};
            IndexKey16 high;
            high.bytes.fill(UINT8_MAX);
            if (lower) {
                low = PrefixBoundary(lower->first, lower->second ? 0U : UINT8_MAX);
            }
            if (upper) {
                high = PrefixBoundary(upper->first, upper->second ? UINT8_MAX : 0U);
            }
            access_path = "index " + chosen_index->name;
            return std::make_unique<IndexRangeScan>(context, table, *chosen_index, low, high, true,
                                                    true);
        }
    }

    if (!IsAggregateQuery(statement.projections, statement.group_by) &&
        statement.where.kind == SqlExpressionKind::EMPTY && statement.order_by.size() == 1U &&
        statement.order_by.front().expression.kind == SqlExpressionKind::COLUMN) {
        const size_t order_column =
            ResolveColumn(statement.order_by.front().expression, table.schema, table.name, alias);
        const auto index = std::find_if(
            table.indexes.begin(), table.indexes.end(), [order_column, &table](const auto& item) {
                return item.column == order_column && IsOrderableIndex(item, table.schema) &&
                       table.not_null[order_column];
            });
        if (index != table.indexes.end()) {
            access_path = "ordered index " + index->name;
            return std::make_unique<IndexOrderScan>(context, table, *index,
                                                    statement.order_by.front().descending);
        }
    }
    access_path = "table heap";
    return std::make_unique<SeqScan>(context, table);
}

QueryPlan BuildSelectPlan(ExecutionContext& context, CatalogTable& table,
                          const SqlStatement& original) {
    SqlStatement statement = original;
    ExpandStar(statement.projections, table.schema);
    QueryPlan plan;
    AggregateList aggregate_definitions;
    const bool aggregate_query = IsAggregateQuery(statement.projections, statement.group_by);
    plan.output_schema = ProjectionSchema(statement.projections, table.schema, table.name,
                                          statement.alias, aggregate_definitions);

    std::unique_ptr<VolcanoOperator> root =
        BuildScan(context, table, statement, statement.alias, plan.access_path);
    if (statement.where.kind != SqlExpressionKind::EMPTY) {
        auto predicate =
            CompileExpression(statement.where, table.schema, table.name, statement.alias);
        root =
            std::make_unique<Filter>(context, std::move(root), table.schema, std::move(predicate));
    }

    if (aggregate_query) {
        aggregate_definitions.clear();
        std::vector<std::unique_ptr<BoundExpression>> projection;
        projection.reserve(statement.projections.size());
        plan.output_schema = ProjectionSchema(statement.projections, table.schema, table.name,
                                              statement.alias, aggregate_definitions);
        for (const SqlSelectItem& item : statement.projections) {
            projection.push_back(CompileExpression(item.expression, table.schema, table.name,
                                                   statement.alias, &aggregate_definitions));
        }
        std::vector<std::unique_ptr<BoundExpression>> group_by;
        for (const SqlExpression& expression : statement.group_by) {
            group_by.push_back(
                CompileExpression(expression, table.schema, table.name, statement.alias));
        }
        root = std::make_unique<Aggregate>(context, std::move(root), table.schema,
                                           plan.output_schema, std::move(projection),
                                           std::move(group_by), std::move(aggregate_definitions));
        if (!statement.order_by.empty()) {
            std::vector<std::unique_ptr<BoundExpression>> keys;
            std::vector<bool> descending;
            for (const SqlOrderItem& item : statement.order_by) {
                const std::optional<size_t> column =
                    OutputColumnForOrder(item.expression, statement.projections);
                if (!column) {
                    throw DbError(ErrorCode::UNKNOWN_COLUMN,
                                  "Aggregate ORDER BY expressions must be projected");
                }
                auto key = std::make_unique<BoundExpression>();
                key->kind = BoundKind::COLUMN;
                key->column = *column;
                keys.push_back(std::move(key));
                descending.push_back(item.descending);
            }
            root = std::make_unique<Sort>(context, std::move(root), plan.output_schema,
                                          std::move(keys), std::move(descending));
        }
        if (statement.limit || statement.offset != 0) {
            root =
                std::make_unique<LimitOffset>(context, std::move(root), statement.limit.value_or(0),
                                              statement.offset, statement.limit.has_value());
        }
    } else {
        if (!statement.order_by.empty()) {
            bool index_order =
                plan.access_path.rfind("ordered index ", 0) == 0 &&
                statement.order_by.size() == 1U &&
                statement.order_by.front().expression.kind == SqlExpressionKind::COLUMN;
            if (!index_order) {
                std::vector<std::unique_ptr<BoundExpression>> keys;
                std::vector<bool> descending;
                for (const SqlOrderItem& item : statement.order_by) {
                    keys.push_back(CompileExpression(item.expression, table.schema, table.name,
                                                     statement.alias));
                    descending.push_back(item.descending);
                }
                root = std::make_unique<Sort>(context, std::move(root), table.schema,
                                              std::move(keys), std::move(descending));
            }
        }
        if (statement.limit || statement.offset != 0) {
            root =
                std::make_unique<LimitOffset>(context, std::move(root), statement.limit.value_or(0),
                                              statement.offset, statement.limit.has_value());
        }
        std::vector<std::unique_ptr<BoundExpression>> projection;
        projection.reserve(statement.projections.size());
        for (const SqlSelectItem& item : statement.projections) {
            projection.push_back(
                CompileExpression(item.expression, table.schema, table.name, statement.alias));
        }
        root = std::make_unique<Projection>(context, std::move(root), table.schema,
                                            plan.output_schema, std::move(projection));
    }
    plan.root = std::move(root);
    for (size_t index = 0; index < statement.projections.size(); ++index) {
        plan.output_names.push_back(ProjectionName(statement.projections[index], index));
    }
    return plan;
}

bool ValuesEqual(const Value& left, const Value& right) {
    const std::optional<int> comparison = CompareValues(left, right);
    return comparison && *comparison == 0;
}

void ValidateTableRow(const CatalogTable& table, const Row& row) {
    ValidateRow(table.schema, row);
    for (size_t column = 0; column < row.size(); ++column) {
        if (table.not_null[column] && IsNullValue(row[column])) {
            throw DbError(ErrorCode::NOT_NULL_VIOLATION,
                          "Column cannot be NULL: " + table.schema.columns[column].name);
        }
    }
}

void CheckUniqueValues(const CatalogTable& table, const Row& row,
                       const std::unordered_set<uint64_t>& excluded_rids) {
    for (const CatalogIndex& index : table.indexes) {
        if (!index.unique || IsNullValue(row[index.column])) {
            continue;
        }
        for (uint64_t packed_rid : FindIndexRids(*index.tree, row[index.column])) {
            if (excluded_rids.find(packed_rid) != excluded_rids.end()) {
                continue;
            }
            const RID rid = RID::Unpack(packed_rid);
            const std::optional<Tuple> tuple = table.heap->Get(rid);
            if (!tuple) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Unique index references a missing row",
                              rid.page_id);
            }
            const Row existing = DeserializeTuple(table.schema, *tuple);
            if (ValuesEqual(existing[index.column], row[index.column])) {
                throw DbError(ErrorCode::DUPLICATE_KEY,
                              "Duplicate value for unique index " + index.name);
            }
        }
    }
}

void RemoveIndexEntries(CatalogTable& table, const Row& row, const RID& rid) {
    for (CatalogIndex& index : table.indexes) {
        if (IsNullValue(row[index.column])) {
            continue;
        }
        if (!index.tree->Delete(IndexEntryKey(row[index.column], rid.Pack()))) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Index entry is missing during row update",
                          index.meta_page_id);
        }
    }
}

void AddIndexEntries(CatalogTable& table, const Row& row, const RID& rid) {
    for (CatalogIndex& index : table.indexes) {
        if (!IsNullValue(row[index.column])) {
            index.tree->Insert(IndexEntryKey(row[index.column], rid.Pack()), rid.Pack());
        }
    }
}

Tuple AffectedCountTuple(uint64_t count) {
    const Schema schema{{Column{"affected_rows", ColumnType::BIGINT}}, std::nullopt};
    return SerializeTuple(schema, Row{static_cast<int64_t>(count)});
}

class Insert final : public VolcanoOperator {
   public:
    Insert(ExecutionContext& context, CatalogTable& table, std::vector<Row> rows)
        : VolcanoOperator(context), table_(table), rows_(std::move(rows)) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + " (" + table_.name + ")";
    }

    uint64_t AffectedRows() const noexcept {
        return affected_;
    }

   protected:
    void InitImpl() override {
        for (Row& row : rows_) {
            if (context_.cancel != nullptr) {
                context_.cancel->Check();
            }
            ValidateTableRow(table_, row);
            CheckUniqueValues(table_, row, {});
            const Tuple tuple = SerializeTuple(table_.schema, row);
            const RID rid = table_.heap->Insert(tuple);
            AddIndexEntries(table_, row, rid);
            ++affected_;
        }
        result_ = AffectedCountTuple(affected_);
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        if (returned_) {
            return false;
        }
        *tuple = result_;
        *rid = RID{};
        returned_ = true;
        return true;
    }

    std::string Name() const override {
        return "Insert";
    }

   private:
    CatalogTable& table_;
    std::vector<Row> rows_;
    Tuple result_;
    uint64_t affected_{0};
    bool returned_{false};
};

struct UpdateAssignment {
    size_t column{0};
    std::unique_ptr<BoundExpression> expression;
};

class Update final : public VolcanoOperator {
   public:
    Update(ExecutionContext& context, CatalogTable& table, std::unique_ptr<VolcanoOperator> input,
           std::vector<UpdateAssignment> assignments)
        : VolcanoOperator(context),
          table_(table),
          input_(std::move(input)),
          assignments_(std::move(assignments)) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + " (" + table_.name + ")\n" +
               input_->Explain(depth + 1U);
    }

    uint64_t AffectedRows() const noexcept {
        return affected_;
    }

   protected:
    void InitImpl() override {
        input_->Init();
        std::vector<std::pair<RID, Row>> pending;
        Tuple tuple;
        RID rid{};
        while (input_->Next(&tuple, &rid)) {
            if (context_.cancel != nullptr) {
                context_.cancel->Check();
            }
            Row updated = DeserializeTuple(table_.schema, tuple);
            const Row original = updated;
            for (const UpdateAssignment& assignment : assignments_) {
                updated[assignment.column] = CoerceValue(assignment.expression->Eval(original),
                                                         table_.schema.columns[assignment.column]);
            }
            ValidateTableRow(table_, updated);
            pending.emplace_back(rid, std::move(updated));
        }

        std::unordered_set<uint64_t> selected;
        selected.reserve(pending.size());
        for (const auto& entry : pending) {
            selected.insert(entry.first.Pack());
        }
        for (size_t row_index = 0; row_index < pending.size(); ++row_index) {
            CheckUniqueValues(table_, pending[row_index].second, selected);
            for (size_t previous = 0; previous < row_index; ++previous) {
                for (const CatalogIndex& index : table_.indexes) {
                    if (index.unique &&
                        ValuesEqual(pending[row_index].second[index.column],
                                    pending[previous].second[index.column]) &&
                        !IsNullValue(pending[row_index].second[index.column])) {
                        throw DbError(ErrorCode::DUPLICATE_KEY,
                                      "UPDATE would create duplicate values for " + index.name);
                    }
                }
            }
        }

        std::vector<Row> old_rows;
        old_rows.reserve(pending.size());
        for (const auto& entry : pending) {
            const std::optional<Tuple> tuple_before = table_.heap->Get(entry.first);
            if (!tuple_before) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Selected row disappeared before UPDATE",
                              entry.first.page_id);
            }
            old_rows.push_back(DeserializeTuple(table_.schema, *tuple_before));
        }
        for (size_t index = 0; index < pending.size(); ++index) {
            RemoveIndexEntries(table_, old_rows[index], pending[index].first);
        }
        for (size_t index = 0; index < pending.size(); ++index) {
            const Tuple updated_tuple = SerializeTuple(table_.schema, pending[index].second);
            const std::optional<RID> updated_rid =
                table_.heap->Update(pending[index].first, updated_tuple);
            if (!updated_rid) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Selected row disappeared during UPDATE",
                              pending[index].first.page_id);
            }
            AddIndexEntries(table_, pending[index].second, *updated_rid);
            ++affected_;
        }
        result_ = AffectedCountTuple(affected_);
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        if (returned_) {
            return false;
        }
        *tuple = result_;
        *rid = RID{};
        returned_ = true;
        return true;
    }

    std::string Name() const override {
        return "Update";
    }

   private:
    CatalogTable& table_;
    std::unique_ptr<VolcanoOperator> input_;
    std::vector<UpdateAssignment> assignments_;
    Tuple result_;
    uint64_t affected_{0};
    bool returned_{false};
};

class Delete final : public VolcanoOperator {
   public:
    Delete(ExecutionContext& context, CatalogTable& table, std::unique_ptr<VolcanoOperator> input)
        : VolcanoOperator(context), table_(table), input_(std::move(input)) {}

    std::string Explain(size_t depth = 0) const override {
        return std::string(depth * 2U, ' ') + Name() + " (" + table_.name + ")\n" +
               input_->Explain(depth + 1U);
    }

    uint64_t AffectedRows() const noexcept {
        return affected_;
    }

   protected:
    void InitImpl() override {
        input_->Init();
        std::vector<std::pair<RID, Row>> pending;
        Tuple tuple;
        RID rid{};
        while (input_->Next(&tuple, &rid)) {
            if (context_.cancel != nullptr) {
                context_.cancel->Check();
            }
            pending.emplace_back(rid, DeserializeTuple(table_.schema, tuple));
        }
        for (const auto& entry : pending) {
            RemoveIndexEntries(table_, entry.second, entry.first);
        }
        for (const auto& entry : pending) {
            if (!table_.heap->Delete(entry.first)) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Selected row disappeared during DELETE",
                              entry.first.page_id);
            }
            ++affected_;
        }
        result_ = AffectedCountTuple(affected_);
    }

    bool NextImpl(Tuple* tuple, RID* rid) override {
        if (returned_) {
            return false;
        }
        *tuple = result_;
        *rid = RID{};
        returned_ = true;
        return true;
    }

    std::string Name() const override {
        return "Delete";
    }

   private:
    CatalogTable& table_;
    std::unique_ptr<VolcanoOperator> input_;
    Tuple result_;
    uint64_t affected_{0};
    bool returned_{false};
};

std::string RenderRows(VolcanoOperator& root, const Schema& schema,
                       const std::vector<std::string>& names) {
    std::ostringstream result;
    for (size_t index = 0; index < names.size(); ++index) {
        if (index != 0) {
            result << '\t';
        }
        result << names[index];
    }
    Tuple tuple;
    RID rid{};
    uint64_t count = 0;
    root.Init();
    while (root.Next(&tuple, &rid)) {
        if (count >= MAX_RESULT_ROWS) {
            throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED, "Query exceeds MAX_RESULT_ROWS");
        }
        const Row row = DeserializeTuple(schema, tuple);
        result << '\n';
        for (size_t index = 0; index < row.size(); ++index) {
            if (index != 0) {
                result << '\t';
            }
            result << FormatSqlValue(row[index]);
        }
        ++count;
    }
    root.Finish();
    return result.str();
}

std::string FormatPlan(const QueryPlan& plan) {
    return "Access path: " + plan.access_path + "\n" + plan.root->Explain();
}

CatalogTable& RequireTable(Catalog& catalog, std::string_view name) {
    CatalogTable* table = catalog.FindTable(name);
    if (table == nullptr) {
        throw DbError(ErrorCode::UNKNOWN_TABLE, "Unknown table: " + std::string(name));
    }
    return *table;
}

size_t RequireColumn(const Schema& schema, std::string_view name) {
    const std::string canonical = Canonical(name);
    for (size_t column = 0; column < schema.columns.size(); ++column) {
        if (Canonical(schema.columns[column].name) == canonical) {
            return column;
        }
    }
    throw DbError(ErrorCode::UNKNOWN_COLUMN, "Unknown column: " + std::string(name));
}

ColumnType ParseColumnType(std::string_view type_name) {
    const std::string type = Canonical(type_name);
    if (type == "int" || type == "integer") {
        return ColumnType::INT;
    }
    if (type == "bigint") {
        return ColumnType::BIGINT;
    }
    if (type == "double" || type == "real") {
        return ColumnType::DOUBLE;
    }
    if (type == "boolean" || type == "bool") {
        return ColumnType::BOOLEAN;
    }
    if (type == "varchar" || type == "text") {
        return ColumnType::VARCHAR;
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT,
                  "Unsupported column type: " + std::string(type_name));
}

void CreateTable(Catalog& catalog, const SqlStatement& statement) {
    if (statement.column_definitions.empty()) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "CREATE TABLE requires at least one column");
    }
    Schema schema;
    std::vector<bool> not_null;
    std::vector<size_t> unique_columns;
    schema.columns.reserve(statement.column_definitions.size());
    not_null.reserve(statement.column_definitions.size());
    for (const SqlColumnDefinition& definition : statement.column_definitions) {
        const ColumnType type = ParseColumnType(definition.type_name);
        const uint16_t length = type == ColumnType::VARCHAR
                                    ? definition.type_length.value_or(uint16_t{255})
                                    : uint16_t{0};
        schema.columns.push_back(Column{std::string(definition.name), type, length});
        bool column_not_null = false;
        for (SqlConstraintKind constraint : definition.constraints) {
            if (constraint == SqlConstraintKind::PRIMARY_KEY) {
                if (schema.primary_key) {
                    throw DbError(ErrorCode::INVALID_ARGUMENT,
                                  "Only one primary-key column is supported");
                }
                schema.primary_key = schema.columns.size() - 1U;
                column_not_null = true;
            } else if (constraint == SqlConstraintKind::NOT_NULL) {
                column_not_null = true;
            } else if (constraint == SqlConstraintKind::UNIQUE) {
                unique_columns.push_back(schema.columns.size() - 1U);
            } else if (constraint == SqlConstraintKind::REFERENCES) {
                throw DbError(ErrorCode::FK_VIOLATION,
                              "Foreign-key constraints are not implemented");
            }
        }
        not_null.push_back(column_not_null);
    }
    if (!statement.primary_key_columns.empty()) {
        if (statement.primary_key_columns.size() != 1U || schema.primary_key) {
            throw DbError(ErrorCode::INVALID_ARGUMENT,
                          "Exactly one primary-key column is supported");
        }
        schema.primary_key = RequireColumn(schema, statement.primary_key_columns.front());
        not_null[*schema.primary_key] = true;
    }
    schema.Validate();
    CatalogTable& table =
        catalog.CreateTable(std::string(statement.table), std::move(schema), std::move(not_null));
    for (size_t column : unique_columns) {
        if (table.schema.primary_key && *table.schema.primary_key == column) {
            continue;
        }
        const std::string index_name = "uq_" + table.name + "_" + table.schema.columns[column].name;
        catalog.CreateIndex(table, index_name, column, true);
    }
}

Value EvaluateValueExpression(const SqlExpression& expression) {
    if (const std::optional<Value> literal = AstLiteral(expression)) {
        return *literal;
    }
    if (expression.kind == SqlExpressionKind::LITERAL) {
        return ParseLiteral(expression.value);
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "INSERT values must be literals");
}

std::vector<Row> BindInsertRows(const CatalogTable& table, const SqlStatement& statement) {
    std::vector<size_t> target_columns;
    if (statement.columns.empty()) {
        target_columns.resize(table.schema.columns.size());
        for (size_t column = 0; column < target_columns.size(); ++column) {
            target_columns[column] = column;
        }
    } else {
        target_columns.reserve(statement.columns.size());
        for (std::string_view name : statement.columns) {
            target_columns.push_back(RequireColumn(table.schema, name));
        }
    }
    std::unordered_set<size_t> distinct_columns;
    for (size_t column : target_columns) {
        if (!distinct_columns.insert(column).second) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "INSERT column is specified more than once");
        }
    }

    std::vector<Row> rows;
    rows.reserve(statement.value_rows.size());
    for (const auto& values : statement.value_rows) {
        if (values.size() != target_columns.size()) {
            throw DbError(ErrorCode::INVALID_ARGUMENT,
                          "INSERT value count does not match its target columns");
        }
        Row row(table.schema.columns.size(), std::monostate{});
        for (size_t value_index = 0; value_index < values.size(); ++value_index) {
            const size_t column = target_columns[value_index];
            row[column] = CoerceValue(EvaluateValueExpression(values[value_index]),
                                      table.schema.columns[column]);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::unique_ptr<VolcanoOperator> BuildDmlInput(ExecutionContext& context, CatalogTable& table,
                                               const SqlStatement& statement,
                                               std::string& access_path) {
    std::unique_ptr<VolcanoOperator> input =
        BuildScan(context, table, statement, statement.alias, access_path);
    if (statement.where.kind != SqlExpressionKind::EMPTY) {
        input = std::make_unique<Filter>(
            context, std::move(input), table.schema,
            CompileExpression(statement.where, table.schema, table.name, statement.alias));
    }
    return input;
}

std::string RenderAffectedRows(VolcanoOperator& root) {
    const Schema result_schema{{Column{"affected_rows", ColumnType::BIGINT}}, std::nullopt};
    Tuple tuple;
    RID rid{};
    root.Init();
    if (!root.Next(&tuple, &rid)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "DML operator did not return an affected-row count");
    }
    root.Finish();
    const Row row = DeserializeTuple(result_schema, tuple);
    return std::to_string(std::get<int64_t>(row.front())) + " row(s) affected";
}

std::string StatementPlan(const SqlStatement& statement, Catalog& catalog,
                          ExecutionContext& context) {
    if (statement.kind == SqlStatementKind::SELECT) {
        CatalogTable& table = RequireTable(catalog, statement.table);
        return FormatPlan(BuildSelectPlan(context, table, statement));
    }
    if (statement.kind == SqlStatementKind::UPDATE || statement.kind == SqlStatementKind::DELETE) {
        CatalogTable& table = RequireTable(catalog, statement.table);
        std::string access_path;
        auto input = BuildDmlInput(context, table, statement, access_path);
        const std::string op = statement.kind == SqlStatementKind::UPDATE ? "Update" : "Delete";
        return "Access path: " + access_path + "\n" + op + " (" + table.name + ")\n" +
               input->Explain(1);
    }
    if (statement.kind == SqlStatementKind::INSERT) {
        return "Insert (" + std::string(statement.table) + ")";
    }
    return "DDL (" + PrintSql(statement) + ")";
}

std::string AnalyzeSelect(QueryPlan& plan, OperatorStatsSink& stats) {
    Tuple tuple;
    RID rid{};
    uint64_t rows = 0;
    plan.root->Init();
    while (plan.root->Next(&tuple, &rid)) {
        if (rows >= MAX_RESULT_ROWS) {
            throw DbError(ErrorCode::RESULT_LIMIT_EXCEEDED, "Query exceeds MAX_RESULT_ROWS");
        }
        ++rows;
    }
    plan.root->Finish();
    std::ostringstream result;
    result << FormatPlan(plan) << "\nActual rows: " << rows << '\n';
    for (const OperatorMetrics& metric : stats.Metrics()) {
        result << metric.name << ": rows=" << metric.rows << " wall_ms=" << std::fixed
               << std::setprecision(3) << static_cast<double>(metric.elapsed_ns) / 1000000.0
               << " pages_fetched=" << metric.pages_fetched
               << " pages_missed=" << metric.pages_missed << '\n';
    }
    return result.str();
}

}  // namespace

class SqlExecutionEngine::Impl {
   public:
    Impl(std::shared_ptr<BufferPoolManager> buffer_pool, WriteAheadLog* wal,
         WriterPreferringSharedMutex& statement_gate, page_id_t owner_meta_page_id)
        : buffer_pool_(std::move(buffer_pool)),
          wal_(wal),
          statement_gate_(statement_gate),
          catalog_(buffer_pool_, owner_meta_page_id) {}

    std::string Execute(std::string_view query) {
        SqlStatement statement;
        try {
            statement = SqlParser(query).Parse();
        } catch (const SqlError& error) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, error.what());
        }
        if (statement.kind == SqlStatementKind::EXPLAIN) {
            return ExecuteExplain(statement);
        }
        if (statement.kind == SqlStatementKind::SELECT ||
            statement.kind == SqlStatementKind::SHOW_TABLES ||
            statement.kind == SqlStatementKind::DESCRIBE) {
            StatementScope scope(statement_gate_, false);
            std::shared_lock<RankedSharedMutex> catalog_lock(catalog_gate_);
            if (statement.kind == SqlStatementKind::SELECT) {
                return ExecuteSelect(statement, &scope.Cancellation(), nullptr);
            }
            return ExecuteReadMetadata(statement);
        }
        StatementScope scope(statement_gate_, true);
        std::unique_lock<RankedSharedMutex> catalog_lock(catalog_gate_);
        catalog_.Reload();
        if (statement.kind == SqlStatementKind::CHECKPOINT) {
            if (wal_ != nullptr) {
                wal_->Checkpoint(*buffer_pool_);
            }
            buffer_pool_->AssertNoPinsHeld();
            return "Checkpoint complete";
        }

        if (wal_ == nullptr) {
            throw DbError(ErrorCode::INVALID_ARGUMENT,
                          "SQL writes require an Executor constructed with a WAL");
        }
        std::shared_ptr<TxnContext> transaction;
        if (wal_ != nullptr) {
            transaction = buffer_pool_->BeginTransaction(*wal_);
        }
        try {
            ExecutionContext context{&catalog_, buffer_pool_.get(), transaction.get(),
                                     &scope.Cancellation(), nullptr};
            const std::string result = ExecuteWrite(statement, context);
            if (transaction) {
                transaction->Commit();
            }
            buffer_pool_->AssertNoPinsHeld();
            return result;
        } catch (const DbError&) {
            if (transaction) {
                transaction->Rollback();
            }
            catalog_.Reload();
            buffer_pool_->AssertNoPinsHeld();
            throw;
        } catch (const std::exception&) {
            if (transaction) {
                transaction->Rollback();
            }
            catalog_.Reload();
            buffer_pool_->AssertNoPinsHeld();
            throw;
        }
    }

   private:
    std::string ExecuteReadMetadata(const SqlStatement& statement) {
        if (statement.kind == SqlStatementKind::SHOW_TABLES) {
            std::ostringstream result;
            result << "Tables";
            for (const auto& table : catalog_.Tables()) {
                result << '\n' << table->name;
            }
            buffer_pool_->AssertNoPinsHeld();
            return result.str();
        }
        const CatalogTable& table = RequireTable(catalog_, statement.table);
        std::ostringstream result;
        result << "column\ttype\tnot_null\tprimary_key";
        for (size_t index = 0; index < table.schema.columns.size(); ++index) {
            const Column& column = table.schema.columns[index];
            result << '\n'
                   << column.name << '\t' << ColumnTypeName(column) << '\t'
                   << (table.not_null[index] ? "YES" : "NO") << '\t'
                   << (table.schema.primary_key == index ? "YES" : "NO");
        }
        buffer_pool_->AssertNoPinsHeld();
        return result.str();
    }

    static std::string ColumnTypeName(const Column& column) {
        switch (column.type) {
            case ColumnType::INT:
                return "INT";
            case ColumnType::BIGINT:
                return "BIGINT";
            case ColumnType::DOUBLE:
                return "DOUBLE";
            case ColumnType::BOOLEAN:
                return "BOOLEAN";
            case ColumnType::VARCHAR:
                return "VARCHAR(" + std::to_string(column.varchar_length) + ")";
        }
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Invalid column type");
    }

    std::string ExecuteSelect(const SqlStatement& statement, CancellationToken* cancellation,
                              OperatorStatsSink* stats) {
        CatalogTable& table = RequireTable(catalog_, statement.table);
        ExecutionContext context{&catalog_, buffer_pool_.get(), nullptr, cancellation, stats};
        QueryPlan plan = BuildSelectPlan(context, table, statement);
        const std::string result = RenderRows(*plan.root, plan.output_schema, plan.output_names);
        buffer_pool_->AssertNoPinsHeld();
        return result;
    }

    std::string ExecuteWrite(const SqlStatement& statement, ExecutionContext& context) {
        switch (statement.kind) {
            case SqlStatementKind::CREATE_TABLE:
                if (statement.if_not_exists && catalog_.FindTable(statement.table) != nullptr) {
                    return "Table already exists";
                }
                CreateTable(catalog_, statement);
                return "Table created";
            case SqlStatementKind::DROP_TABLE:
                catalog_.DropTable(statement.table);
                return "Table dropped";
            case SqlStatementKind::CREATE_INDEX: {
                CatalogTable& table = RequireTable(catalog_, statement.table);
                if (statement.index_columns.size() != 1U) {
                    throw DbError(ErrorCode::INVALID_ARGUMENT,
                                  "Only single-column indexes are supported");
                }
                const size_t column = RequireColumn(table.schema, statement.index_columns.front());
                catalog_.CreateIndex(table, std::string(statement.name), column, statement.flag);
                return "Index created";
            }
            case SqlStatementKind::INSERT: {
                CatalogTable& table = RequireTable(catalog_, statement.table);
                Insert operation(context, table, BindInsertRows(table, statement));
                const std::string result = RenderAffectedRows(operation);
                buffer_pool_->AssertNoPinsHeld();
                return result;
            }
            case SqlStatementKind::UPDATE: {
                CatalogTable& table = RequireTable(catalog_, statement.table);
                std::string access_path;
                auto input = BuildDmlInput(context, table, statement, access_path);
                std::vector<UpdateAssignment> assignments;
                for (const SqlAssignment& assignment : statement.assignments) {
                    UpdateAssignment bound;
                    bound.column = RequireColumn(table.schema, assignment.column);
                    bound.expression = CompileExpression(assignment.expression, table.schema,
                                                         table.name, statement.alias);
                    assignments.push_back(std::move(bound));
                }
                Update operation(context, table, std::move(input), std::move(assignments));
                const std::string result = RenderAffectedRows(operation);
                buffer_pool_->AssertNoPinsHeld();
                return result;
            }
            case SqlStatementKind::DELETE: {
                CatalogTable& table = RequireTable(catalog_, statement.table);
                std::string access_path;
                auto input = BuildDmlInput(context, table, statement, access_path);
                Delete operation(context, table, std::move(input));
                const std::string result = RenderAffectedRows(operation);
                buffer_pool_->AssertNoPinsHeld();
                return result;
            }
            default:
                throw DbError(ErrorCode::INVALID_ARGUMENT, "Statement is not a write statement");
        }
    }

    std::string ExecuteExplain(const SqlStatement& statement) {
        if (!statement.explained_statement) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "EXPLAIN is missing its statement");
        }
        const SqlStatement& explained = *statement.explained_statement;
        const bool write = explained.kind != SqlStatementKind::SELECT;
        StatementScope scope(statement_gate_, write);
        std::unique_lock<RankedSharedMutex> catalog_write_lock(catalog_gate_, std::defer_lock);
        std::shared_lock<RankedSharedMutex> catalog_read_lock(catalog_gate_, std::defer_lock);
        if (write) {
            catalog_write_lock.lock();
        } else {
            catalog_read_lock.lock();
        }
        if (write) {
            catalog_.Reload();
        }
        ExecutionContext context{&catalog_, buffer_pool_.get(), nullptr, &scope.Cancellation(),
                                 nullptr};
        if (!statement.flag) {
            const std::string plan = StatementPlan(explained, catalog_, context);
            buffer_pool_->AssertNoPinsHeld();
            return plan;
        }
        if (explained.kind == SqlStatementKind::SELECT) {
            CatalogTable& table = RequireTable(catalog_, explained.table);
            OperatorStatsSink stats;
            context.stats = &stats;
            QueryPlan plan = BuildSelectPlan(context, table, explained);
            const std::string result = AnalyzeSelect(plan, stats);
            buffer_pool_->AssertNoPinsHeld();
            return result;
        }
        if (wal_ == nullptr) {
            throw DbError(ErrorCode::INVALID_ARGUMENT,
                          "EXPLAIN ANALYZE of writes requires a WAL-backed Executor");
        }
        std::shared_ptr<TxnContext> transaction;
        if (wal_ != nullptr) {
            transaction = buffer_pool_->BeginTransaction(*wal_);
        }
        try {
            OperatorStatsSink stats;
            ExecutionContext write_context{&catalog_, buffer_pool_.get(), transaction.get(),
                                           &scope.Cancellation(), &stats};
            const std::string affected = ExecuteWrite(explained, write_context);
            if (transaction) {
                transaction->Commit();
            }
            std::ostringstream output;
            output << "Access path: executed write statement\n" << affected << '\n';
            for (const OperatorMetrics& metric : stats.Metrics()) {
                output << metric.name << ": rows=" << metric.rows
                       << " wall_ms=" << static_cast<double>(metric.elapsed_ns) / 1000000.0
                       << " pages_fetched=" << metric.pages_fetched
                       << " pages_missed=" << metric.pages_missed << '\n';
            }
            buffer_pool_->AssertNoPinsHeld();
            return output.str();
        } catch (const DbError&) {
            if (transaction) {
                transaction->Rollback();
            }
            catalog_.Reload();
            buffer_pool_->AssertNoPinsHeld();
            throw;
        } catch (const std::exception&) {
            if (transaction) {
                transaction->Rollback();
            }
            catalog_.Reload();
            buffer_pool_->AssertNoPinsHeld();
            throw;
        }
    }

    std::shared_ptr<BufferPoolManager> buffer_pool_;
    WriteAheadLog* wal_;
    WriterPreferringSharedMutex& statement_gate_;
    RankedSharedMutex catalog_gate_{LockRank::CATALOG};
    Catalog catalog_;
};

SqlExecutionEngine::SqlExecutionEngine(std::shared_ptr<BufferPoolManager> buffer_pool,
                                       WriteAheadLog* wal,
                                       WriterPreferringSharedMutex& statement_gate,
                                       page_id_t owner_meta_page_id)
    : impl_(std::make_unique<Impl>(std::move(buffer_pool), wal, statement_gate,
                                   owner_meta_page_id)) {}

SqlExecutionEngine::~SqlExecutionEngine() = default;

std::string SqlExecutionEngine::Execute(std::string_view query) {
    return impl_->Execute(query);
}

}  // namespace relational_db
