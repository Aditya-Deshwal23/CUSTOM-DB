#!/usr/bin/env python3
"""Capture real API samples and query responses for the dashboard replay mode."""

import argparse
import datetime
import json
import os
import tempfile
import time
import urllib.parse
import urllib.error
import urllib.request


def request_json(base_url, path, payload=None):
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        base_url.rstrip("/") + path,
        data=body,
        headers={"Accept": "application/json", "Content-Type": "application/json"},
        method="POST" if body is not None else "GET",
    )
    try:
        response = urllib.request.urlopen(request, timeout=30)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.status, json.loads(response.read())


def capture_catalog(base_url):
    status, payload = request_json(base_url, "/api/v1/tables")
    if status != 200 or not isinstance(payload.get("tables"), list):
        raise RuntimeError(f"Catalog request failed with HTTP {status}: {payload}")
    tables = []
    for name in payload["tables"]:
        status, detail = request_json(
            base_url, f"/api/v1/tables/{urllib.parse.quote(name, safe='')}"
        )
        if status != 200:
            raise RuntimeError(f"Table metadata request for {name} failed with HTTP {status}")
        tables.append(detail)
    return tables


def capture_sample(base_url, elapsed_ms):
    status, stats = request_json(base_url, "/api/v1/stats")
    if status != 200:
        raise RuntimeError(f"Stats request failed with HTTP {status}: {stats}")
    status, buffer_pool = request_json(base_url, "/api/v1/stats/buffer_pool")
    if status != 200:
        raise RuntimeError(f"Buffer-pool request failed with HTTP {status}: {buffer_pool}")
    status, storage = request_json(base_url, "/api/v1/stats/storage")
    if status != 200:
        raise RuntimeError(f"Storage request failed with HTTP {status}: {storage}")
    status, version = request_json(base_url, "/api/v1/version")
    if status != 200:
        raise RuntimeError(f"Version request failed with HTTP {status}: {version}")
    return {
        "elapsed_ms": elapsed_ms,
        "stats": stats,
        "buffer_pool": buffer_pool,
        "storage": storage,
        "version": version,
        "catalog": capture_catalog(base_url),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080", help="Live engine base URL")
    parser.add_argument("--duration-seconds", type=float, default=60)
    parser.add_argument("--interval-seconds", type=float, default=1)
    parser.add_argument(
        "--query",
        action="append",
        default=[],
        help="SQL to execute and record; may be supplied more than once",
    )
    parser.add_argument(
        "--output",
        default=os.path.join("dashboard", "replay", "session.json"),
        help="Replay JSON output path",
    )
    args = parser.parse_args()
    if args.duration_seconds <= 0 or args.interval_seconds <= 0:
        parser.error("duration and interval must be greater than zero")

    started = time.monotonic()
    recorded_at = datetime.datetime.now(datetime.timezone.utc).isoformat()
    queries = []
    for sql in args.query:
        elapsed_ms = int((time.monotonic() - started) * 1000)
        status, response = request_json(args.url, "/api/v1/query", {"sql": sql})
        queries.append(
            {"elapsed_ms": elapsed_ms, "sql": sql, "status": status, "response": response}
        )

    samples = []
    while True:
        elapsed = time.monotonic() - started
        if elapsed > args.duration_seconds and samples:
            break
        samples.append(capture_sample(args.url, int(elapsed * 1000)))
        remaining = args.duration_seconds - (time.monotonic() - started)
        if remaining <= 0:
            break
        time.sleep(min(args.interval_seconds, remaining))

    recording = {
        "source": "real-engine-run",
        "recorded_at": recorded_at,
        "duration_ms": int((time.monotonic() - started) * 1000),
        "samples": samples,
        "queries": queries,
    }
    output = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=os.path.dirname(output), delete=False
        ) as temporary:
            temporary_path = temporary.name
            json.dump(recording, temporary, indent=2)
            temporary.write("\n")
        os.replace(temporary_path, output)
    except OSError:
        if temporary_path and os.path.exists(temporary_path):
            os.unlink(temporary_path)
        raise
    print(f"Recorded {len(samples)} live samples and {len(queries)} query responses to {output}")


if __name__ == "__main__":
    main()
