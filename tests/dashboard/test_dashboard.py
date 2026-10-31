import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

from playwright.sync_api import sync_playwright


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
SERVER = os.environ.get(
    "RELDB_SERVER_BINARY", os.path.join(ROOT, "build-debug", "relational_db")
)


def request_json(base_url, path, payload=None):
    body = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(
        base_url + path,
        data=body,
        headers={"Content-Type": "application/json"} if body else {},
        method="POST" if body else "GET",
    )
    try:
        response = urllib.request.urlopen(request, timeout=3)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.status, json.loads(response.read())


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def start_server(port, database):
    environment = os.environ.copy()
    environment.update({"PORT": str(port), "DB_PATH": database})
    return subprocess.Popen(
        [SERVER], cwd=ROOT, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )


def wait_ready(process, base_url, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"Engine exited with status {process.returncode}")
        try:
            status, health = request_json(base_url, "/healthz")
            if status == 200 and health.get("ok"):
                return
        except (OSError, TimeoutError, urllib.error.URLError):
            time.sleep(0.1)
    raise AssertionError("Engine did not become ready")


def test_real_dashboard_query_offline_restart_and_recovery(tmp_path):
    port = free_port()
    base_url = f"http://127.0.0.1:{port}"
    database = str(tmp_path / "dashboard-smoke.db")
    process = start_server(port, database)
    restarted = None
    try:
        wait_ready(process, base_url)
        status, created = request_json(
            base_url, "/api/v1/query", {"sql": "CREATE TABLE browser_smoke (id INT PRIMARY KEY, label VARCHAR(32))"}
        )
        assert status == 200 and created["ok"]
        status, inserted = request_json(
            base_url, "/api/v1/query", {"sql": "INSERT INTO browser_smoke VALUES (1, 'real row')"}
        )
        assert status == 200 and inserted["rows_affected"] == 1
        hostile_text = "<img src=x onerror=1>"
        status, inserted = request_json(
            base_url,
            "/api/v1/query",
            {"sql": f"INSERT INTO browser_smoke VALUES (2, '{hostile_text}')"},
        )
        assert status == 200 and inserted["rows_affected"] == 1

        with sync_playwright() as playwright:
            browser = playwright.chromium.launch()
            page = browser.new_page(viewport={"width": 360, "height": 800})
            page_errors = []
            failed_requests = []
            page.on("pageerror", lambda error: page_errors.append(str(error)))
            page.on("requestfailed", lambda request: failed_requests.append(
                f"{request.method} {request.url}: {request.failure}"
            ))
            page.goto(base_url, wait_until="networkidle")
            page.locator("#statusText").get_by_text("LIVE ENGINE", exact=True).wait_for(timeout=10000)
            try:
                page.locator(".schema-table h3").filter(has_text="browser_smoke").wait_for(timeout=5000)
            except Exception as error:
                raise AssertionError(
                    f"Catalog did not render; title={page.locator('#schemaSummary').inner_text()!r}; "
                    f"offline={page.locator('#offlineMessage').inner_text()!r}; "
                    f"page_errors={page_errors!r}; failed_requests={failed_requests!r}"
                ) from error
            assert page.evaluate("document.documentElement.scrollWidth <= document.documentElement.clientWidth")

            page.locator("#sqlInput").fill("SELECT id, label FROM browser_smoke WHERE id = 1")
            page.locator("#sqlInput").press("Control+Enter")
            page.locator("#resultTable tbody tr").wait_for(timeout=10000)
            assert page.locator("#resultTable tbody tr").inner_text() == "1\treal row"
            page.locator("#qpsSparklinePath[d^='M']").wait_for(timeout=10000)
            page.locator("#sqlInput").fill("SELECT label FROM browser_smoke WHERE id = 2")
            page.locator("#sqlInput").press("Control+Enter")
            page.get_by_role("cell", name=hostile_text, exact=True).wait_for(timeout=10000)
            assert page.locator("#resultTable tbody td").last.inner_text() == hostile_text
            assert page.locator("#resultTable img").count() == 0
            page.locator("#sqlInput").fill("")
            page.locator("#sqlInput").press("ArrowUp")
            assert page.locator("#sqlInput").input_value() == "SELECT label FROM browser_smoke WHERE id = 2"
            page.locator("#sqlInput").fill(
                "EXPLAIN ANALYZE SELECT id FROM browser_smoke WHERE id = 1"
            )
            page.locator("#sqlInput").press("Control+Enter")
            page.locator(".operator-table tbody tr").first.wait_for(timeout=10000)
            assert page.locator("#pipe-parser").get_attribute("class").endswith("active")
            page.locator("#sqlInput").fill("SELECT FROM browser_smoke")
            page.locator("#sqlInput").press("Control+Enter")
            page.locator(".error-location mark").wait_for(timeout=10000)

            process.send_signal(signal.SIGKILL)
            process.wait(timeout=5)
            page.get_by_role("alert").wait_for(timeout=8000)
            assert "Offline" in page.locator("#offlineTitle").inner_text() or \
                "Waking up" in page.locator("#offlineTitle").inner_text()
            assert page.locator("#cacheHitRate").inner_text() == "—"

            restarted = start_server(port, database)
            wait_ready(restarted, base_url)
            status, storage = request_json(base_url, "/api/v1/stats/storage")
            assert status == 200 and storage["recovery"]["ran"] is True
            page.locator("#retryButton").click()
            page.get_by_text("LIVE ENGINE", exact=True).wait_for(timeout=10000)
            page.locator("#recoverySummary").get_by_text("Ran", exact=False).wait_for()
            browser.close()
    finally:
        for server in (process, restarted):
            if server is not None and server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)


def test_empty_catalog_runs_editable_fk_seed_through_query_api(tmp_path):
    port = free_port()
    base_url = f"http://127.0.0.1:{port}"
    process = start_server(port, str(tmp_path / "empty-catalog.db"))
    try:
        wait_ready(process, base_url)
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch()
            page = browser.new_page(viewport={"width": 1280, "height": 900})
            page.goto(base_url, wait_until="networkidle")
            page.locator("#seedPanel").wait_for(state="visible", timeout=10000)
            script = page.locator("#seedScript")
            assert "REFERENCES Users(id)" in script.input_value()
            page.locator("#loadDemoButton").click()
            try:
                page.get_by_text("4 catalog tables", exact=True).wait_for(timeout=20000)
            except Exception as error:
                raise AssertionError(
                    f"Seed did not render catalog: status={page.locator('#seedStatus').inner_text()!r}; "
                    f"schema={page.locator('#schemaSummary').inner_text()!r}"
                ) from error
            page.locator(".schema-table").filter(has_text="OrderItems").get_by_text(
                "FK → Products.id", exact=True
            ).wait_for()
            browser.close()
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


def test_recorder_writes_real_api_samples(tmp_path):
    port = free_port()
    base_url = f"http://127.0.0.1:{port}"
    database = str(tmp_path / "recording.db")
    output = tmp_path / "session.json"
    process = start_server(port, database)
    try:
        wait_ready(process, base_url)
        status, created = request_json(
            base_url, "/api/v1/query", {"sql": "CREATE TABLE record_rows (id INT PRIMARY KEY)"}
        )
        assert status == 200 and created["ok"]
        status, inserted = request_json(
            base_url, "/api/v1/query", {"sql": "INSERT INTO record_rows VALUES (7)"}
        )
        assert status == 200 and inserted["rows_affected"] == 1
        subprocess.run(
            [
                sys.executable,
                os.path.join(ROOT, "scripts", "record_session.py"),
                "--url",
                base_url,
                "--duration-seconds",
                "0.2",
                "--interval-seconds",
                "0.1",
                "--query",
                "SELECT id FROM record_rows",
                "--output",
                str(output),
            ],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        )
        recording = json.loads(output.read_text(encoding="utf-8"))
        assert recording["source"] == "real-engine-run"
        assert recording["samples"][0]["version"]["git_sha"]
        assert recording["samples"][0]["catalog"][0]["name"] == "record_rows"
        assert recording["queries"][0]["response"]["rows"] == [[7]]
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
