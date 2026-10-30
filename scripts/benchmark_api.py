#!/usr/bin/env python3
"""Measure deterministic INSERT and SELECT API latency using only Python's standard library."""

import argparse
import http.client
import json
import statistics
import time
from urllib.parse import urlparse


def percentile(samples, fraction):
    ordered = sorted(samples)
    return ordered[int((len(ordered) - 1) * fraction)]


def request(connection, query):
    payload = json.dumps({"query": query})
    started = time.perf_counter_ns()
    connection.request("POST", "/api/query", payload, {"Content-Type": "application/json"})
    response = connection.getresponse()
    body = response.read().decode("utf-8")
    latency_ms = (time.perf_counter_ns() - started) / 1_000_000
    if response.status != 200:
        raise RuntimeError(f"{query!r} returned HTTP {response.status}: {body}")
    return json.loads(body)["result"], latency_ms


def report(label, samples, elapsed_seconds):
    print(
        f"{label}: requests={len(samples)} wall_s={elapsed_seconds:.3f} "
        f"throughput_ops_s={len(samples) / elapsed_seconds:.1f} "
        f"avg_ms={statistics.fmean(samples):.3f} p50_ms={percentile(samples, 0.50):.3f} "
        f"p95_ms={percentile(samples, 0.95):.3f} p99_ms={percentile(samples, 0.99):.3f}"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--insert-count", type=int, default=1000)
    parser.add_argument("--read-runs", type=int, default=3)
    parser.add_argument("--start-key", type=int, default=1)
    args = parser.parse_args()

    parsed = urlparse(args.base_url)
    if parsed.scheme != "http" or not parsed.hostname:
        raise SystemExit("--base-url must be an HTTP URL, for example http://127.0.0.1:8080")

    connection = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=10)
    insert_latencies = []
    insert_started = time.perf_counter()
    for key in range(args.start_key, args.start_key + args.insert_count):
        expected = f"Inserted ({key}, {key * 10})"
        result, latency = request(connection, f"INSERT {key} {key * 10}")
        if result != expected:
            raise RuntimeError(f"expected {expected!r}, received {result!r}")
        insert_latencies.append(latency)
    report("insert", insert_latencies, time.perf_counter() - insert_started)

    for run in range(1, args.read_runs + 1):
        read_latencies = []
        read_started = time.perf_counter()
        for offset in range(args.insert_count):
            key = args.start_key + ((offset * 37) % args.insert_count)
            expected = f"Result: {key * 10}"
            result, latency = request(connection, f"SELECT {key}")
            if result != expected:
                raise RuntimeError(f"expected {expected!r}, received {result!r}")
            read_latencies.append(latency)
        report(f"read_run_{run}", read_latencies, time.perf_counter() - read_started)
    connection.close()


if __name__ == "__main__":
    main()
