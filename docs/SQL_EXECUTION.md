# SQL execution subset

The parser, catalog, planner, and executor implement a deliberate SQL subset,
not general SQL. The execution API is the same `POST /api/query` route and the
legacy integer `INSERT <key> <value>` / `SELECT <key>` commands remain
available.

## Supported surface

- `CREATE TABLE`, `DROP TABLE`, `CREATE [UNIQUE] INDEX`, `INSERT`, `UPDATE`,
  `DELETE`, `SELECT`, `SHOW TABLES`, `DESCRIBE`, `EXPLAIN`, and
  `EXPLAIN ANALYZE`.
- INT, BIGINT, DOUBLE, BOOLEAN, and bounded VARCHAR columns; primary key,
  NOT NULL, and unique-index checks.
- Predicates with comparisons, AND/OR/NOT, IN, BETWEEN, LIKE, and IS NULL;
  arithmetic expressions; projection; ORDER BY; LIMIT/OFFSET; COUNT, SUM,
  MIN, MAX, AVG, and hash GROUP BY.
- Point/range index access for supported indexed scalar types, sequential
  scan fallback, and ordered index scans where the planner can preserve the
  requested order.
- One auto-commit WAL transaction per write statement. UPDATE and DELETE
  identify target rows before modifying them, and all indexes are maintained
  when rows change or move.

`EXPLAIN` displays the planned operator tree without executing it.
`EXPLAIN ANALYZE` executes the SELECT and reports actual rows, inclusive
operator wall time, buffer-pool page-fetch/miss deltas, and the selected access
path. It is not a trace of dashboard simulation behavior.

The catalog is currently serialized into one page. Foreign keys, composite
indexes/primary keys, multi-statement transactions, SQL joins, and general
transaction isolation are not implemented. VARCHAR equality indexes use a
64-bit hash prefix and verify candidate rows; VARCHAR index range scans and
index ordering are not supported. Query results are rendered as tab/newline
text for the existing API.

The engine enforces `MAX_RESULT_ROWS`, `MAX_SORT_ROWS`, and a cooperative
statement timeout (`STATEMENT_TIMEOUT_MS`). A timeout check is performed while
operators scan; it cannot interrupt a single blocking disk operation.

## SQLite differential test

`tools/difftest.py` uses only Python's standard library. It starts the service
with a temporary database, generates a deterministic mixed workload, applies
each statement to the service and `sqlite3`, and compares query results. CI
runs 10,000 generated statements; reproduce with:

```sh
python3 tools/difftest.py --binary build/relational_db --statements 10000
```

The seed is printed and can be overridden with `--seed`. The harness uses
`DURABILITY=off` only to disable the periodic WAL sync thread; every statement
commit still syncs its WAL record before returning.

The generated subset avoids or normalizes known engine/SQLite semantic
differences:

- Integer division is not generated; SQLite and this engine do not promise
  identical division result types/rounding.
- LIKE inputs are lowercase ASCII, avoiding SQLite's default ASCII
  case-insensitive LIKE behavior versus the engine's case-sensitive matching.
- Unordered results are compared without relying on row order. Ordered queries
  use the unique primary key as their order key, so LIMIT/OFFSET select the same
  rows. NULL ordering is therefore not used as a comparison oracle.
- SQL BOOLEAN values are normalized to 0/1 when comparing with SQLite's
  integer representation.
- DOUBLE values use a small relative/absolute tolerance for aggregate
  floating-point rounding; generated DOUBLE values are finite decimal values.

These are generator constraints, not claims that the engine matches SQLite
outside the supported subset. The CI job and focused SQL tests are listed in
[`CLAIMS.md`](./CLAIMS.md).
