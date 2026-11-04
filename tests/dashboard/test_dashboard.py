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


def post_sql(base_url, sql):
    return request_json(base_url, "/api/v1/query", {"sql": sql})


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def start_server(port, database, settings=None):
    environment = os.environ.copy()
    environment.update({"PORT": str(port), "DB_PATH": database})
    if settings:
        environment.update({key: str(value) for key, value in settings.items()})
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


def test_studio_explorer_table_views_and_atomic_inline_edit(tmp_path):
    port = free_port()
    base_url = f"http://127.0.0.1:{port}"
    process = start_server(port, str(tmp_path / "studio.db"), {"READ_ONLY": "0"})
    try:
        wait_ready(process, base_url)
        assert post_sql(
            base_url,
            "CREATE TABLE studio_parent (id INT PRIMARY KEY, label VARCHAR(32) NOT NULL)",
        )[0] == 200
        assert post_sql(
            base_url,
            "CREATE TABLE studio_child (id INT PRIMARY KEY, parent_id INT REFERENCES studio_parent(id))",
        )[0] == 200
        assert post_sql(
            base_url,
            "INSERT INTO studio_parent VALUES " +
            ", ".join(f"({value}, 'label {value}')" for value in range(1, 61)),
        )[0] == 200
        assert post_sql(base_url, "INSERT INTO studio_child VALUES (1, 1)")[0] == 200

        with sync_playwright() as playwright:
            browser = playwright.chromium.launch()
            context = browser.new_context(permissions=["clipboard-read", "clipboard-write"])
            context.add_init_script(
                """(() => {
                    let copiedText = "";
                    Object.defineProperty(navigator, "clipboard", {
                        value: {
                            writeText: async (text) => {
                                copiedText = text;
                            },
                            readText: async () => copiedText
                        }
                    });
                })();"""
            )
            context.add_init_script(path=os.path.join(ROOT, "node_modules/axe-core/axe.min.js"))
            page = context.new_page()
            page.set_viewport_size({"width": 1440, "height": 1000})
            page.goto(base_url, wait_until="networkidle")
            page.locator("#explorerTree .tree-table-summary").filter(
                has_text="studio_parent"
            ).wait_for(timeout=10000)
            page.locator("#explorerFilter").fill("studio_parent")
            assert page.locator("#explorerTree .tree-table-summary").filter(
                has_text="studio_parent"
            ).count() == 1
            page.locator("#explorerFilter").fill("")
            tree_list = page.locator("#explorerTree .tree-table-list")
            tree_list.focus()
            page.keyboard.press("ArrowDown")
            assert page.evaluate("document.activeElement.matches('.tree-table-summary')")
            page.keyboard.press("Enter")
            page.wait_for_function(
                "() => document.querySelector('#studioView h2')?.textContent === 'studio_parent'"
            )
            assert page.locator("#studioView h2").inner_text() == "studio_parent"

            page.locator("#explorerTree .tree-table-summary").filter(
                has_text="studio_parent"
            ).click(button="right")
            page.get_by_role("menuitem", name="Open data").click()
            grid = page.get_by_role("grid", name="studio_parent rows")
            grid.wait_for()
            page.get_by_label("Rows per page").select_option("50")
            page.wait_for_function(
                "() => document.querySelector('[role=grid]')?.getAttribute('aria-rowcount') === '51'"
            )
            assert grid.get_attribute("aria-rowcount") == "51"
            assert 0 < page.locator(".grid-row").count() < 50
            page.get_by_role("button", name="Next").click()
            page.get_by_text("Rows 51–60 of ~60 (estimate)").wait_for()
            assert page.locator(".grid-row").count() == 10
            page.get_by_role("button", name="Previous").click()

            page.get_by_role("button", name="Add filter").click()
            filter_row = page.locator(".filter-row").last
            filter_row.locator("select").nth(0).select_option("id")
            filter_row.locator("select").nth(1).select_option("=")
            filter_row.locator("input").fill("59")
            page.get_by_role("button", name="Apply filters").click()
            page.get_by_role("gridcell", name="label 59", exact=True).wait_for()
            grid = page.get_by_role("grid", name="studio_parent rows")
            grid.focus()
            page.keyboard.press("ArrowRight")
            assert page.evaluate("document.activeElement.matches('.grid-cell')")
            page.get_by_role("gridcell", name="label 59", exact=True).click()
            page.get_by_role("button", name="Copy selection").click()
            assert "label 59" in page.evaluate("navigator.clipboard.readText()")
            page.get_by_role("button", name="Copy as").click()
            assert "label 59" in page.evaluate("navigator.clipboard.readText()")
            with page.expect_download():
                page.get_by_role("button", name="Export page").click()

            for subtab in ["Structure", "Indexes", "Constraints", "DDL", "Storage", "Relations"]:
                page.get_by_role("button", name=subtab, exact=True).click()
                page.locator(".table-subtabs button[aria-current=page]").get_by_text(
                    subtab, exact=True
                ).wait_for()
                if subtab == "Indexes":
                    page.get_by_role("button", name="Open B+ tree").click()
                    page.locator("#indexTreePanel pre").wait_for()
            page.get_by_role("button", name="Relations", exact=True).click()
            first_paths = page.locator(".er-edge").evaluate_all(
                "(paths) => paths.map((path) => path.getAttribute('d'))"
            )
            page.get_by_role("button", name="Data", exact=True).click()
            page.locator("#explorerTree .tree-table-summary").filter(
                has_text="studio_child"
            ).click(button="right")
            page.get_by_role("menuitem", name="Structure").click()
            page.get_by_role("button", name="parent.id").click()
            page.get_by_role("grid", name="studio_parent rows").wait_for()

            page.locator("#explorerTree .tree-table-summary").filter(
                has_text="studio_parent"
            ).click(button="right")
            page.get_by_role("menuitem", name="Select top 100").click()
            assert page.locator("#sqlInput").input_value() == "SELECT * FROM studio_parent LIMIT 100"

            page.locator("#explorerTree .tree-table-summary").filter(
                has_text="studio_parent"
            ).click(button="right")
            page.get_by_role("menuitem", name="Open data").click()
            page.locator(".grid-row").first.get_by_role("gridcell").nth(2).dblclick()
            editor = page.locator(".cell-editor")
            editor.fill("updated")
            editor.press("Enter")
            page.get_by_role("button", name="Review SQL").click()
            page.get_by_role("dialog").get_by_text(
                "UPDATE studio_parent SET label = ? WHERE id = ?", exact=True
            ).wait_for()
            page.get_by_role("button", name="Close").click()
            page.get_by_role("button", name="Apply", exact=True).click()
            page.get_by_role("gridcell", name="updated", exact=True).wait_for(timeout=10000)

            page.locator("#erTab").click()
            page.locator(".er-canvas svg").wait_for()
            rendered_paths = page.locator(".er-edge").evaluate_all(
                "(paths) => paths.map((path) => path.getAttribute('d'))"
            )
            assert rendered_paths == first_paths
            accessibility = page.evaluate("""async () => {
                const result = await axe.run(document.querySelector('.studio-shell'));
                return result.violations
                    .filter((violation) => ['serious', 'critical'].includes(violation.impact))
                    .map((violation) => ({
                        id: violation.id,
                        impact: violation.impact,
                        nodes: violation.nodes.map((node) => ({
                            target: node.target,
                            label: node.target[0] &&
                                document.querySelector(node.target[0])?.getAttribute("aria-label"),
                            text: node.target[0] &&
                                document.querySelector(node.target[0])?.textContent
                        }))
                    }));
            }""")
            assert accessibility == [], accessibility
            grid_probe = page.evaluate("""async () => {
                const longTasks = [];
                const observer = new PerformanceObserver((entries) => {
                    longTasks.push(...entries.getEntries().map((entry) => entry.duration));
                });
                observer.observe({ type: 'longtask', buffered: true });
                const { createVirtualGrid } = await import('/js/grid.js');
                const host = document.createElement('div');
                document.body.appendChild(host);
                const rows = Array.from({ length: 10000 }, (_, id) => ({
                    rid: `${id + 1}:0`, values: { id: id + 1, label: `row ${id + 1}` }
                }));
                const grid = createVirtualGrid(host, [
                    { name: 'id', type: 'INT' }, { name: 'label', type: 'VARCHAR' }
                ], rows);
                const before = host.querySelectorAll('.grid-row').length;
                grid.viewport.scrollTop = 320000;
                grid.viewport.dispatchEvent(new Event('scroll'));
                await new Promise((resolve) => requestAnimationFrame(() =>
                    requestAnimationFrame(resolve)));
                observer.disconnect();
                return {
                    before,
                    after: host.querySelectorAll('.grid-row').length,
                    total: Number(grid.root.getAttribute('aria-rowcount')),
                    longTasks
                };
            }""")
            assert grid_probe["total"] == 10001
            assert grid_probe["before"] <= 50
            assert grid_probe["after"] <= 50
            assert not any(duration > 50 for duration in grid_probe["longTasks"])
            browser.close()
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


def test_studio_read_only_mode_disables_drop_and_edits(tmp_path):
    port = free_port()
    base_url = f"http://127.0.0.1:{port}"
    database = str(tmp_path / "studio-readonly.db")
    writable = start_server(port, database, {"READ_ONLY": "0"})
    readonly = None
    try:
        wait_ready(writable, base_url)
        assert post_sql(
            base_url,
            "CREATE TABLE readonly_rows (id INT PRIMARY KEY, label VARCHAR(20))",
        )[0] == 200
        assert post_sql(base_url, "INSERT INTO readonly_rows VALUES (1, 'fixed')")[0] == 200
        writable.terminate()
        writable.wait(timeout=5)
        readonly = start_server(port, database, {"READ_ONLY": "1"})
        wait_ready(readonly, base_url)

        with sync_playwright() as playwright:
            browser = playwright.chromium.launch()
            page = browser.new_page()
            page.goto(base_url, wait_until="networkidle")
            page.get_by_text("READ ONLY", exact=True).wait_for(timeout=10000)
            page.locator("#explorerTree .tree-table-summary").filter(
                has_text="readonly_rows"
            ).click(button="right")
            drop = page.get_by_role("menuitem", name="Drop table…")
            assert drop.is_disabled()
            page.get_by_role("menuitem", name="Open data").click()
            page.get_by_text("Read-only mode: cell editing is unavailable.").wait_for()
            assert page.get_by_role("button", name="Review SQL").count() == 0
            browser.close()
    finally:
        for server in (writable, readonly):
            if server is not None and server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
