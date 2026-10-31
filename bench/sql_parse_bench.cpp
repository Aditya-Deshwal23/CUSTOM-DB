#include <benchmark/benchmark.h>

#include <string_view>

#include "sql_parser.h"

namespace {

constexpr std::string_view kQuery =
    "SELECT account_id, balance + 12.5 AS adjusted FROM accounts "
    "WHERE status = 'active' AND balance BETWEEN 100 AND 100000 "
    "ORDER BY account_id LIMIT 50";

void BM_SqlLexer(benchmark::State& state) {
    for (auto _ : state) {
        const auto tokens = relational_db::SqlLexer::Tokenize(kQuery);
        benchmark::DoNotOptimize(tokens.data());
        benchmark::DoNotOptimize(tokens.size());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kQuery.size()));
}

void BM_SqlParser(benchmark::State& state) {
    for (auto _ : state) {
        relational_db::SqlStatement statement = relational_db::SqlParser(kQuery).Parse();
        benchmark::DoNotOptimize(&statement);
        benchmark::DoNotOptimize(statement.projections.data());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kQuery.size()));
}

BENCHMARK(BM_SqlLexer);
BENCHMARK(BM_SqlParser);

}  // namespace

BENCHMARK_MAIN();
