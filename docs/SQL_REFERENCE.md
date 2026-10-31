# SQL reference

CUSTOM-DB implements a versioned subset, not general SQL. The parser and
execution details are maintained in [SQL_EXECUTION.md](SQL_EXECUTION.md);
the executable API contract is [openapi.yaml](openapi.yaml).

## Statements

Supported statement families:

```sql
CREATE TABLE people (
  id INT PRIMARY KEY,
  name VARCHAR(64) NOT NULL,
  mentor_id INT REFERENCES people(id)
);
CREATE [UNIQUE] INDEX people_name ON people(name);
INSERT INTO people VALUES (1, 'Ada', NULL);
UPDATE people SET name = 'Ada Lovelace' WHERE id = 1;
DELETE FROM people WHERE id = 1;
SELECT id, name FROM people WHERE id >= 1 ORDER BY id LIMIT 20 OFFSET 0;
SHOW TABLES;
DESCRIBE people;
EXPLAIN SELECT id FROM people WHERE id = 1;
EXPLAIN ANALYZE SELECT id FROM people WHERE id = 1;
DROP TABLE people;
```

Supported types are `INT`, `BIGINT`, `DOUBLE`, `BOOLEAN`, and bounded
`VARCHAR(n)`. Constraints include primary key, `NOT NULL`, unique indexes, and
column-level foreign keys to a unique column of the same type.

Expressions include comparisons, `AND`/`OR`/`NOT`, `IN`, `BETWEEN`, `LIKE`,
`IS NULL`, arithmetic, and supported aggregates (`COUNT`, `SUM`, `MIN`,
`MAX`, `AVG`) with hash `GROUP BY`. The planner may choose point/range index
access, ordered index access, or a sequential scan. `ORDER BY`, `LIMIT`, and
`OFFSET` are supported in the documented forms. See the tests in
[sql_parser_test.cpp](../tests/sql_parser_test.cpp) and
[sql_execution_test.cpp](../tests/sql_execution_test.cpp).

## HTTP query form

The primary endpoint is `POST /api/v1/query`:

```sh
curl -fsS -X POST http://localhost:8080/api/v1/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT id FROM people WHERE id = $1","params":[1]}'
```

Success responses contain typed column metadata and row arrays. Errors contain
a stable error code and request ID. The deprecated `query` JSON member and
`/api/query` route remain compatibility paths. Full schemas, errors, route
limits, and examples are in [openapi.yaml](openapi.yaml).

## Semantic and resource boundaries

- Each DDL/DML statement is a separate auto-commit WAL transaction. There is
  no SQL `BEGIN`/`COMMIT` or multi-statement transaction API.
- Joins, table-level/composite foreign keys, composite SQL indexes, and general
  transaction isolation are not implemented.
- `VARCHAR` is limited to 255 bytes; the encoded tuple must fit in one heap
  page (maximum 4,067 bytes). No overflow pages exist.
- VARCHAR indexes support equality candidates with row rechecks, not ordered
  index ranges.
- Sorting and hash grouping use memory; there is no spill-to-disk operator.
- Cancellation is cooperative in executor loops and cannot interrupt a
  blocking operating-system I/O call.

The full list is in [LIMITATIONS.md](LIMITATIONS.md). Differential testing
compares a generated supported subset with SQLite and intentionally avoids
known semantic differences; it is not proof of full SQLite compatibility.
