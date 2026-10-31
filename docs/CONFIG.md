# Runtime configuration

The service reads its environment once at startup into a validated `Config`.
Invalid configuration is reported as a JSON error on stderr and exits with
status 78. On a normal service boot, the effective configuration is logged as
JSON; `API_KEY` is always redacted. An empty value is treated as unset unless
the table says otherwise.

| Variable | Default | Accepted range / values |
|---|---:|---|
| `PORT` | `8080` | Integer `1`–`65535`. The container image defaults to `8080`; Render supplies `10000`. |
| `DB_PATH` | `./test.db` | Non-empty filesystem path. The image and Compose use `/data/custom.db`. |
| `WAL_PATH` | `${DB_PATH}-wal` | Non-empty filesystem path different from `DB_PATH`; unset or empty selects the default. |
| `BUFFER_POOL_FRAMES` | `4096` | Integer `16`–`2147483647` (the signed 32-bit frame-ID limit). |
| `BUFFER_POOL_POLICY` | `lruk` | `clock` or `lruk`. |
| `DURABILITY` | `full` | `full`, `normal`, or `off`. `off` requires `ALLOW_UNSAFE=1`; write commits still sync their WAL record. |
| `WAL_MAX_BYTES` | `16777216` | Positive integer bytes, `1`–`18446744073709551615`. |
| `WAL_SYNC_INTERVAL_MS` | `1000` | Integer `1`–`600000`. Used by `normal` mode. |
| `READ_ONLY` | `0` | `0` or `1`; in read-only mode SQL mutation statements are rejected. |
| `API_KEY` | unset | Empty or at most 4096 bytes. When set, SQL writes and administrative checkpoints require the `X-API-Key` header. |
| `CORS_ORIGINS` | empty | Comma-separated HTTP(S) origins without paths, queries, or fragments; empty disables cross-origin access. |
| `MAX_BODY_BYTES` | `65536` | Integer `1`–`16777216`. |
| `MAX_SQL_BYTES` | `16384` | Integer `1`–`16777216`. |
| `MAX_RESULT_ROWS` | `1000` | Integer `1`–`100000`. |
| `STATEMENT_TIMEOUT_MS` | `2000` | Integer `1`–`600000`. |
| `RATE_LIMIT_RPS` | `20` | Finite number greater than `0` and at most `1000000`. |
| `RATE_LIMIT_BURST` | `40` | Finite number `1`–`1000000`. This companion setting is retained for the existing per-IP token bucket. |
| `MAX_DB_BYTES` | `0` (disabled) | `0` disables the quota; otherwise any positive unsigned 64-bit byte count. |
| `LOG_LEVEL` | `info` | `debug`, `info`, `warn`, `error`, or `off`. |
| `DEMO_LOAD` | `0` | `0` or `1`; enables the optional in-process point-lookup workload. |
| `SLOW_QUERY_MS` | `0` (disabled) | Integer `0`–`3600000`. |
| `DRAIN_TIMEOUT_MS` | `5000` | Integer `1`–`600000`; bounds the graceful in-flight request drain. |
| `ALLOW_UNSAFE` | `0` | `0` or `1`; must be `1` to select `DURABILITY=off`. |

`MAX_DB_BYTES=0` is the explicit no-quota value. Numeric settings reject
non-integers, overflow, and out-of-range values rather than silently falling
back to defaults. The example container settings are in
[`.env.example`](../.env.example).
