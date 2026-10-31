#!/usr/bin/env python3
"""Compare warm primary-key reads through CUSTOM-DB's HTTP API and Python SQLite."""

import argparse
import json
import os
import platform
import random
import socket
import sqlite3
import statistics
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def percentile(samples, fraction):
    ordered = sorted(samples)
    index = min(len(ordered) - 1, int((len(ordered) - 1) * fraction))
    return ordered[index] / 1_000.0


def timed_reads(read_one, keys, expected):
    samples = []
    started = time.perf_counter()
    for key in keys:
        before = time.perf_counter_ns()
        if read_one(key) != expected[key]:
            raise RuntimeError(f"incorrect result for key {key}")
        samples.append(time.perf_counter_ns() - before)
    elapsed = time.perf_counter() - started
    return {
        "operations": len(keys),
        "elapsed_seconds": elapsed,
        "ops_per_second": len(keys) / elapsed,
        "p50_microseconds": percentile(samples, 0.50),
        "p95_microseconds": percentile(samples, 0.95),
    }


def request_json(url, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"} if data is not None else {},
        method="POST" if data is not None else "GET",
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read())


def run_once(args, seed):
    rng = random.Random(seed)
    keys = [rng.randrange(args.rows) for _ in range(args.iterations)]
    expected = {key: key * 3 for key in range(args.rows)}

    with tempfile.TemporaryDirectory(prefix="custom-db-sqlite-benchmark-") as directory:
        db_path = os.path.join(directory, "custom.db")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        environment = os.environ.copy()
        environment.update(
            {
                "DB_PATH": db_path,
                "PORT": str(port),
                "DURABILITY": "full",
                "RATE_LIMIT_RPS": "1000000",
                "RATE_LIMIT_BURST": "1000000",
            }
        )
        process = subprocess.Popen(
            [args.binary], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        sqlite_path = os.path.join(directory, "sqlite.db")
        sqlite = sqlite3.connect(sqlite_path)
        try:
            base_url = f"http://127.0.0.1:{port}"
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("CUSTOM-DB exited during startup")
                try:
                    request_json(base_url + "/readyz")
                    break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.05)
            else:
                raise RuntimeError("CUSTOM-DB did not become ready")

            request_json(
                base_url + "/api/v1/query",
                {"sql": "CREATE TABLE bench (id INT PRIMARY KEY, payload BIGINT)"},
            )
            sqlite.execute("PRAGMA journal_mode=WAL")
            sqlite.execute("PRAGMA synchronous=FULL")
            sqlite.execute("CREATE TABLE bench (id INTEGER PRIMARY KEY, payload INTEGER)")
            batch_size = 50
            for first in range(0, args.rows, batch_size):
                last = min(first + batch_size, args.rows)
                values = ", ".join(f"({key}, {key * 3})" for key in range(first, last))
                request_json(
                    base_url + "/api/v1/query",
                    {"sql": f"INSERT INTO bench VALUES {values}"},
                )
                sqlite.executemany(
                    "INSERT INTO bench VALUES (?, ?)",
                    ((key, key * 3) for key in range(first, last)),
                )
            sqlite.commit()

            def engine_read(key):
                response = request_json(
                    base_url + "/api/v1/query",
                    {"sql": f"SELECT payload FROM bench WHERE id = {key}"},
                )
                return int(response["rows"][0][0])

            def sqlite_read(key):
                row = sqlite.execute(
                    "SELECT payload FROM bench WHERE id = ?", (key,)
                ).fetchone()
                if row is None:
                    raise RuntimeError(f"SQLite returned no row for key {key}")
                return int(row[0])

            for key in keys[: min(100, len(keys))]:
                engine_read(key)
                sqlite_read(key)

            return {
                "seed": seed,
                "custom_db_http_api": timed_reads(engine_read, keys, expected),
                "sqlite_python_api": timed_reads(sqlite_read, keys, expected),
            }
        finally:
            sqlite.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def summarize(runs, backend):
    fields = ("ops_per_second", "p50_microseconds", "p95_microseconds")
    return {
        field: statistics.median(run[backend][field] for run in runs) for field in fields
    }


def run(args):
    runs = [run_once(args, args.seed + index) for index in range(args.repetitions)]
    return {
        "workload": {
            "rows": args.rows,
            "point_reads_per_engine_per_run": args.iterations,
            "repetitions": args.repetitions,
            "seed_start": args.seed,
            "cache": "warm application buffer pool / warm SQLite connection",
            "storage": "temporary file-backed databases; setup excluded",
        },
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python_version": platform.python_version(),
            "sqlite_version": sqlite3.sqlite_version,
        },
        "custom_db_http_api_median": summarize(runs, "custom_db_http_api"),
        "sqlite_python_api_median": summarize(runs, "sqlite_python_api"),
        "runs": runs,
        "methodology_note": (
            "End-to-end Python HTTP/JSON requests for CUSTOM-DB versus in-process "
            "Python sqlite3 calls; this is not an engine-core apples-to-apples "
            "comparison and includes CUSTOM-DB service/serialization overhead."
        ),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build-release/relational_db")
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--seed", type=int, default=1337)
    args = parser.parse_args()
    if args.rows < 1 or args.iterations < 1 or args.repetitions < 1:
        parser.error("--rows, --iterations, and --repetitions must be positive")
    print(json.dumps(run(args), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
