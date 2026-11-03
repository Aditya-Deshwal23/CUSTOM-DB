#!/usr/bin/env python3
import argparse
import http.client
import json
import math
import os
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def allocate_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def post_sql(base_url, sql):
    request = urllib.request.Request(
        base_url + "/api/v1/query",
        data=json.dumps({"sql": sql}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        response = urllib.request.urlopen(request, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    result = json.loads(response.read())
    if response.status != 200:
        raise RuntimeError(f"database setup failed: HTTP {response.status}: {result}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--samples", type=int, default=100)
    parser.add_argument("--max-p99-ms", type=float, default=2.0)
    args = parser.parse_args()
    if args.samples < 100:
        parser.error("--samples must be at least 100 for a p99 measurement")

    port = allocate_port()
    with tempfile.TemporaryDirectory(prefix="custom-db-snapshot-bench-") as directory:
        env = os.environ.copy()
        env.update(
            {
                "PORT": str(port),
                "DB_PATH": os.path.join(directory, "snapshot.db"),
                "LOG_LEVEL": "off",
            }
        )
        process = subprocess.Popen(
            [args.binary], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
        )
        base_url = f"http://127.0.0.1:{port}"
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    stderr = process.stderr.read().decode(errors="replace")
                    raise RuntimeError(f"server exited during startup: {stderr}")
                try:
                    urllib.request.urlopen(base_url + "/healthz", timeout=0.2).close()
                    break
                except (urllib.error.URLError, TimeoutError):
                    time.sleep(0.05)
            else:
                raise TimeoutError("server did not start")

            post_sql(base_url, "CREATE TABLE snapshot_bench (id INT PRIMARY KEY, label VARCHAR(32))")
            values = ", ".join(f"({row}, 'row-{row}')" for row in range(100))
            post_sql(base_url, f"INSERT INTO snapshot_bench VALUES {values}")

            host, port_number = base_url.removeprefix("http://").split(":")
            connection = http.client.HTTPConnection(host, int(port_number), timeout=3)
            for _ in range(3):
                connection.request("GET", "/api/v1/snapshot")
                response = connection.getresponse()
                response.read()
                if response.status != 200:
                    raise RuntimeError(f"snapshot warm-up returned HTTP {response.status}")

            durations_ms = []
            for _ in range(args.samples):
                time.sleep(0.205)
                started = time.perf_counter_ns()
                connection.request("GET", "/api/v1/snapshot")
                response = connection.getresponse()
                response.read()
                elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
                if response.status != 200:
                    raise RuntimeError(f"snapshot returned HTTP {response.status}")
                durations_ms.append(elapsed_ms)
            connection.close()

            ordered = sorted(durations_ms)
            p99_index = math.ceil(0.99 * len(ordered)) - 1
            p99_ms = ordered[p99_index]
            print(
                f"snapshot warm samples={len(ordered)} p99={p99_ms:.3f}ms "
                f"max={ordered[-1]:.3f}ms median={ordered[len(ordered) // 2]:.3f}ms"
            )
            if p99_ms >= args.max_p99_ms:
                raise RuntimeError(
                    f"snapshot p99 {p99_ms:.3f}ms does not meet "
                    f"the {args.max_p99_ms:.3f}ms limit"
                )
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
