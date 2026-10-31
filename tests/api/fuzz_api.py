import argparse
import json
import os
import random
import socket
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
import urllib.error
import urllib.request


def allocate_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def send(base_url, body, content_type="application/json", timeout=30):
    request = urllib.request.Request(
        base_url + "/api/v1/query",
        data=body,
        headers={"Content-Type": content_type},
        method="POST",
    )
    try:
        response = urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        response = error
    status = response.status
    response.read()
    return status


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--seed", type=int, default=731291)
    parser.add_argument("--concurrent", action="store_true")
    parser.add_argument("--request-timeout", type=float, default=30)
    args = parser.parse_args()
    print(f"API fuzz seed={args.seed} iterations={args.iterations}", flush=True)

    generator = random.Random(args.seed)
    port = allocate_port()
    with tempfile.TemporaryDirectory(prefix="custom-db-api-fuzz-") as directory:
        env = os.environ.copy()
        env.update(
            {
                "PORT": str(port),
                "DB_PATH": os.path.join(directory, "fuzz.db"),
                "RATE_LIMIT_RPS": "100000",
                "RATE_LIMIT_BURST": "100000",
                "LOG_LEVEL": "off",
            }
        )
        stderr_path = os.path.join(directory, "server.stderr.log")
        stderr_log = open(stderr_path, "wb")
        process = subprocess.Popen(
            [args.binary], env=env, stdout=subprocess.DEVNULL, stderr=stderr_log
        )
        base_url = f"http://127.0.0.1:{port}"
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    stderr_log.flush()
                    with open(stderr_path, encoding="utf-8") as log_file:
                        stderr_output = log_file.read()
                    raise RuntimeError(
                        f"server exited during startup: {stderr_output}"
                    )
                try:
                    urllib.request.urlopen(base_url + "/healthz", timeout=0.2).close()
                    break
                except (urllib.error.URLError, TimeoutError):
                    time.sleep(0.05)
            else:
                raise TimeoutError("server did not start")

            alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789' ;$()/*-"

            requests = []
            for index in range(args.iterations):
                if index % 3 == 0:
                    query = "".join(
                        generator.choice(alphabet)
                        for _ in range(generator.randrange(0, 384))
                    )
                    requests.append(
                        (
                            json.dumps(
                                {"sql": query, "params": [generator.randrange(-1000, 1000)]}
                            ).encode(),
                            "application/json",
                        )
                    )
                elif index % 3 == 1:
                    requests.append(
                        (
                            bytes(
                                generator.randrange(0, 256)
                                for _ in range(generator.randrange(0, 128))
                            ),
                            "application/json",
                        )
                    )
                else:
                    payload = generator.choice(
                        [
                            {},
                            {"sql": None},
                            {"sql": ["SELECT 1"]},
                            {"query": "SELECT * FROM x; DROP TABLE x"},
                            {"sql": "SELECT * FROM absent WHERE id = $1", "params": [{}]},
                        ]
                    )
                    requests.append((json.dumps(payload).encode(), "application/json"))

            def submit(index):
                body, content_type = requests[index]
                status = send(
                    base_url, body, content_type, timeout=args.request_timeout
                )
                if status >= 500:
                    raise AssertionError(f"request {index} returned HTTP {status}")
                return status

            if args.concurrent:
                with ThreadPoolExecutor(max_workers=min(8, args.iterations)) as pool:
                    futures = [
                        pool.submit(submit, index) for index in range(args.iterations)
                    ]
                    for future in as_completed(futures):
                        future.result()
                        if process.poll() is not None:
                            raise AssertionError("server crashed during concurrent requests")
            else:
                for index in range(args.iterations):
                    submit(index)
                    if process.poll() is not None:
                        raise AssertionError(f"server crashed after request {index}")

            health = urllib.request.urlopen(base_url + "/healthz", timeout=3)
            if health.status != 200:
                raise AssertionError(f"health check returned HTTP {health.status}")
            print(f"API fuzz passed: {args.iterations} requests; no server crash")
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            stderr_log.close()


if __name__ == "__main__":
    main()
