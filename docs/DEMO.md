# Three-minute dashboard demo

Use the real service rather than a static frontend server:

```sh
cmake --build build --parallel
PORT=8080 DB_PATH=./demo.db ./build/relational_db
```

Open `http://localhost:8080`. For the demo seed, start with an empty database
and leave the service writable (`READ_ONLY=0`, the default).

## Walkthrough

| Time | Action |
|---|---|
| 0:00–0:25 | Point out the `LIVE` build SHA, process uptime, and actual database size. Show the windowed hit rate, query rate, latency percentiles, I/O latency, and active statements/gate waiters. Values remain blank until real samples exist. |
| 0:25–0:45 | Inspect the buffer-pool cells and hover one to show its frame, page ID, pin/dirty state, and clock-hand marker. Read the page/free-page/WAL/checkpoint/recovery summary. |
| 0:45–1:10 | If the catalog is empty, inspect or edit the visible seed SQL and run it. The Users, Products, Orders, and OrderItems DDL/DML statements go through `POST /api/v1/query`; Orders and OrderItems declare enforced `REFERENCES` constraints. |
| 1:10–1:35 | Run `SELECT id, username, email FROM Users;` and show typed result columns and rows. Press Up in the console to recall the last query. |
| 1:35–2:00 | Run `EXPLAIN ANALYZE SELECT id, status FROM Orders WHERE id = 1;`. Expand the operator tree and inspect per-operator rows, wall time, fetched pages, and misses. The execution pipeline only lights stages supported by those counters. |
| 2:00–2:20 | Run `SELECT FROM Orders;` to show the engine's syntax-error code and highlighted line/column. Try `INSERT INTO Orders VALUES (99, 999, 1.0, 'paid');` to see an enforced foreign-key violation. |
| 2:20–3:00 | Stop the server abruptly, show the explicit offline/waking state and blank telemetry, then restart it with the same `DB_PATH`. Retry and show the recovery summary and persisted catalog/data. |

To capture a recording from a live run for a labeled, read-only replay:

```sh
python3 scripts/record_session.py \
  --duration-seconds 60 \
  --interval-seconds 1 \
  --query "EXPLAIN ANALYZE SELECT id FROM Users WHERE id = 1"
```

Open `http://localhost:8080/?replay=1`. Replay requires the generated
`dashboard/replay/session.json` file and always displays its recording date and
real-engine provenance.

The browser smoke test is `tests/dashboard/test_dashboard.py`. The dashboard
honesty guard is `scripts/check_dashboard_honesty.py`; acceptance and evidence
are tracked in [CLAIMS.md](./CLAIMS.md).

## Container crash-recovery proof

Build the production image and run the live hard-kill recovery exercise:

```sh
docker build -t custom-db:local .
scripts/demo_crash.sh
```

The script writes through the HTTP API, sends `SIGKILL` during the write loop,
restarts against the same named Docker volume, prints the recovery log and
`--check` result, verifies recovered rows, and then checks a clean SIGTERM
shutdown. Set `VOLUME`, `HOST_PORT`, or `IMAGE` to select an isolated named
volume, local port, or image tag. The script leaves the named volume in place
for inspection.

The checked-in [asciinema v2 capture](assets/recovery.cast) records the real
container run: 1,752 acknowledged writes survived, with one additional
committed-but-unacknowledged row also recovered. It includes the hard kill,
recovery log, `--check` output, and graceful stop. After building
`custom-db:local`, regenerate it with:

```sh
python3 scripts/record_crash_demo.py --image custom-db:local
```
