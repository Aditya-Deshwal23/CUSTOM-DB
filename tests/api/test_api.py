import contextlib
import json
import os
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request

import pytest
import yaml
from openapi_spec_validator import validate
from prometheus_client.parser import text_string_to_metric_families


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
SERVER = os.environ.get(
    "RELDB_SERVER_BINARY", os.path.join(ROOT, "build-debug", "relational_db")
)


@contextlib.contextmanager
def running_server(tmp_path, extra_env=None, db_path=None):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]

    env = os.environ.copy()
    env.pop("DEMO_LOAD", None)
    env.update({"PORT": str(port), "DB_PATH": str(db_path or tmp_path / "api.db")})
    env.update(extra_env or {})
    process = subprocess.Popen(
        [SERVER], cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )
    base_url = f"http://127.0.0.1:{port}"
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            pytest.fail(f"server exited during startup: {process.stderr.read().decode()}")
        try:
            http_request(base_url, "/healthz", timeout=0.2)
            break
        except (urllib.error.URLError, TimeoutError):
            time.sleep(0.05)
    else:
        process.terminate()
        pytest.fail("server did not start")

    try:
        yield base_url, process
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


@pytest.fixture
def api_server(tmp_path):
    with running_server(tmp_path) as server:
        yield server[0]


def http_request(base_url, path, payload=None, headers=None, raw=None, method=None, timeout=3):
    if raw is not None:
        data = raw
    elif payload is None:
        data = None
    else:
        data = json.dumps(payload).encode()
    request_headers = dict(headers or {})
    if payload is not None and "Content-Type" not in request_headers:
        request_headers["Content-Type"] = "application/json"
    request = urllib.request.Request(
        base_url + path,
        data=data,
        headers=request_headers,
        method=method or ("POST" if data is not None else "GET"),
    )
    try:
        response = urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        response = error
    return response.status, response.read(), response.headers


def request_json(base_url, path, payload=None, headers=None, raw=None, method=None):
    status, body, response_headers = http_request(
        base_url, path, payload, headers, raw, method
    )
    return status, json.loads(body), response_headers


def post_sql(base_url, sql, params=None, headers=None):
    payload = {"sql": sql}
    if params is not None:
        payload["params"] = params
    return request_json(base_url, "/api/v1/query", payload, headers)


def test_health_readiness_and_version_routes(api_server):
    health_status, health, _ = request_json(api_server, "/healthz")
    ready_status, ready, _ = request_json(api_server, "/readyz")
    version_status, version, _ = request_json(api_server, "/api/v1/version")

    assert health_status == 200 and health["ok"] is True
    assert ready_status == 200 and ready["ok"] is True
    assert version_status == 200
    assert {"git_sha", "build_time", "format_version"} <= version.keys()

    static_status, _, static_headers = http_request(api_server, "/")
    cached_status, cached_body, cached_headers = http_request(
        api_server, "/", headers={"If-None-Match": static_headers["ETag"]}
    )
    assert static_status == 200 and static_headers["ETag"]
    assert cached_status == 304 and cached_body == b""
    assert cached_headers["X-Content-Type-Options"] == "nosniff"


def test_query_validation_and_status_mapping(api_server):
    baseline_status, baseline, _ = request_json(api_server, "/api/v1/stats")
    assert baseline_status == 200
    missing_status, missing, _ = request_json(api_server, "/api/v1/query", {})
    wrong_type_status, wrong_type, _ = request_json(
        api_server, "/api/v1/query", {"sql": ["SELECT 1"]}
    )
    syntax_status, syntax, _ = post_sql(api_server, "SELECT FROM")
    unknown_status, unknown, _ = post_sql(api_server, "SELECT * FROM absent_table")
    injection_status, injection, _ = post_sql(
        api_server, "SELECT * FROM missing; DROP TABLE absent_table"
    )
    huge_sql_status, huge_sql, _ = post_sql(
        api_server, "SELECT '" + ("x" * 17000) + "'"
    )

    assert missing_status == 400 and missing["ok"] is False
    assert wrong_type_status == 400 and wrong_type["ok"] is False
    assert syntax_status == 400 and syntax["error"]["code"] == "SYNTAX_ERROR"
    assert unknown_status == 404 and unknown["error"]["code"] == "UNKNOWN_TABLE"
    assert injection_status == 400 and injection["ok"] is False
    assert huge_sql_status == 413 and huge_sql["error"]["code"] == "SQL_TOO_LARGE"
    assert "request_id" in syntax
    assert post_sql(api_server, "CREATE TABLE api_semantic (id INT)")[0] == 200
    type_status, type_error, _ = post_sql(
        api_server, "INSERT INTO api_semantic VALUES ('wrong')"
    )
    assert type_status == 400 and type_error["ok"] is False
    semantic_status, semantic_error, _ = post_sql(
        api_server, "CREATE TABLE semantic_bad (id unsupported_type)"
    )
    assert semantic_status == 422 and semantic_error["ok"] is False
    semantic_status, semantic, _ = post_sql(
        api_server, "SELECT absent_column FROM api_semantic"
    )
    assert semantic_status == 422 and semantic["error"]["code"] == "UNKNOWN_COLUMN"
    header_status, _, response_headers = post_sql(
        api_server, "SELECT FROM", headers={"X-Request-Id": "client-request-42"}
    )
    assert header_status == 400
    assert response_headers["X-Request-Id"] == "client-request-42"
    assert response_headers["X-Content-Type-Options"] == "nosniff"
    assert response_headers["X-Frame-Options"] == "DENY"
    final_status, final, _ = request_json(api_server, "/api/v1/stats")
    assert final_status == 200
    assert final["errors"] > baseline["errors"]
    assert final["errors_by_code"]["unknown_table"] - baseline["errors_by_code"]["unknown_table"] == 1


def test_sql_result_parameters_tables_and_stats(api_server):
    baseline_status, baseline, _ = request_json(api_server, "/api/v1/stats")
    assert baseline_status == 200
    assert post_sql(api_server, "CREATE TABLE api_contract (id INT PRIMARY KEY, label VARCHAR(40))")[0] == 200
    assert post_sql(api_server, "INSERT INTO api_contract VALUES ($1, $2)", [17, "safe'value"])[0] == 200

    status, result, _ = post_sql(
        api_server, "SELECT id, label FROM api_contract WHERE id = $1", [17]
    )
    assert status == 200 and result["ok"] is True
    assert result["statement"] == "SELECT"
    assert result["columns"] == [{"name": "id", "type": "INT"}, {"name": "label", "type": "VARCHAR"}]
    assert result["rows"] == [[17, "safe'value"]]
    assert result["row_count"] == 1
    assert result["rows_affected"] == 0
    assert result["truncated"] is False
    assert isinstance(result["elapsed_us"], int)

    update_status, update, _ = post_sql(
        api_server, "UPDATE api_contract SET label = $1 WHERE id = $2", ["updated", 17]
    )
    assert update_status == 200 and update["rows_affected"] == 1

    tables_status, tables, _ = request_json(api_server, "/api/v1/tables")
    detail_status, detail, _ = request_json(api_server, "/api/v1/tables/api_contract")
    stats_status, stats, _ = request_json(api_server, "/api/v1/stats")
    assert tables_status == 200 and "api_contract" in tables["tables"]
    assert detail_status == 200 and detail["row_estimate"] == 1
    assert detail["schema"][0]["name"] == "id"
    assert any(index["unique"] for index in detail["indexes"])
    assert stats_status == 200 and stats["buffer_pool"]["hits"] >= 0
    assert stats["statements"]["ddl"] - baseline["statements"]["ddl"] == 1
    assert stats["statements"]["insert"] - baseline["statements"]["insert"] == 1
    assert stats["statements"]["select"] - baseline["statements"]["select"] == 1
    assert stats["statements"]["update"] - baseline["statements"]["update"] == 1
    assert stats["rows_read"] - baseline["rows_read"] == 2
    assert stats["rows_written"] - baseline["rows_written"] == 2
    buffer_status, buffer_stats, _ = request_json(api_server, "/api/v1/stats/buffer_pool")
    storage_status, storage_stats, _ = request_json(api_server, "/api/v1/stats/storage")
    assert buffer_status == 200 and buffer_stats["snapshot_size"] == buffer_stats["pool_size"]
    assert storage_status == 200
    assert storage_stats["file_size_bytes"] == storage_stats["page_count"] * 4096
    assert storage_stats["recovery"]["ran"] is True


def test_malformed_json_body_limit_and_content_type(api_server, tmp_path):
    malformed_status, malformed, _ = request_json(
        api_server, "/api/v1/query", raw=b"{", headers={"Content-Type": "application/json"}
    )
    wrong_type_status, wrong_type, _ = request_json(
        api_server,
        "/api/v1/query",
        raw=b'{"sql":"SELECT 1"}',
        headers={"Content-Type": "text/plain"},
    )
    assert malformed_status == 400 and malformed["error"]["code"] == "INVALID_JSON"
    assert wrong_type_status == 415

    with running_server(tmp_path, {"MAX_BODY_BYTES": "64"}) as (limited_url, _):
        body_status, body, _ = request_json(
            limited_url,
            "/api/v1/query",
            raw=b" " * 80,
            headers={"Content-Type": "application/json"},
        )
        assert body_status == 413 and body["error"]["code"] == "BODY_TOO_LARGE"


def test_read_only_api_key_checkpoint_and_cors(tmp_path):
    with running_server(
        tmp_path,
        {"READ_ONLY": "1", "API_KEY": "test-key", "CORS_ORIGINS": "https://allowed.example"},
    ) as (base_url, _):
        readonly_status, readonly, _ = post_sql(base_url, "CREATE TABLE denied (id INT)")
        assert readonly_status == 401 and readonly["error"]["code"] == "API_KEY_REQUIRED"
        legacy_status, _, _ = request_json(
            base_url, "/api/query", {"query": "INSERT 71 710"}
        )
        assert legacy_status == 401

        assert post_sql(base_url, "CREATE TABLE secured (id INT)")[0] == 401
        created_status, _, _ = post_sql(
            base_url, "CREATE TABLE secured (id INT)", headers={"X-API-Key": "test-key"}
        )
        assert created_status == 403

        checkpoint_status, _, _ = http_request(
            base_url, "/api/v1/admin/checkpoint", raw=b"", method="POST"
        )
        assert checkpoint_status == 401
        authorized_status, authorized, _ = request_json(
            base_url,
            "/api/v1/admin/checkpoint",
            payload={},
            headers={"X-API-Key": "test-key"},
        )
        assert authorized_status == 200 and authorized["ok"] is True

        cors_status, _, cors_headers = http_request(
            base_url,
            "/api/v1/query",
            raw=b"",
            method="OPTIONS",
            headers={
                "Origin": "https://allowed.example",
                "Access-Control-Request-Method": "POST",
            },
        )
        assert cors_status == 204
        assert cors_headers["Access-Control-Allow-Origin"] == "https://allowed.example"
        denied_cors_status, denied_cors, _ = request_json(
            base_url,
            "/healthz",
            raw=b"",
            method="OPTIONS",
            headers={
                "Origin": "https://blocked.example",
                "Access-Control-Request-Method": "GET",
            },
        )
        assert denied_cors_status == 403
        assert denied_cors["error"]["code"] == "CORS_ORIGIN_DENIED"

    with running_server(tmp_path, {"API_KEY": "write-key"}) as (base_url, _):
        assert post_sql(base_url, "CREATE TABLE keyed (id INT)")[0] == 401
        allowed_status, _, _ = post_sql(
            base_url, "CREATE TABLE keyed (id INT)", headers={"X-API-Key": "write-key"}
        )
        assert allowed_status == 200


def test_result_limit_and_constraint_status(tmp_path):
    with running_server(tmp_path, {"MAX_RESULT_ROWS": "1"}) as (base_url, _):
        assert post_sql(base_url, "CREATE TABLE limited (id INT PRIMARY KEY)")[0] == 200
        assert post_sql(base_url, "INSERT INTO limited VALUES (1), (2)")[0] == 200
        status, result, _ = post_sql(base_url, "SELECT id FROM limited")
        assert status == 200
        assert result["rows"] == [[1]]
        assert result["row_count"] == 1 and result["truncated"] is True
        assert post_sql(base_url, "INSERT INTO limited VALUES (1)")[0] == 409


def test_database_quota_returns_507(tmp_path):
    with running_server(tmp_path, {"MAX_DB_BYTES": "1"}) as (base_url, _):
        status, result, _ = post_sql(
            base_url, "CREATE TABLE quota_limited (id INT PRIMARY KEY)"
        )
        assert status == 507
        assert result["error"]["code"] == "STORAGE_QUOTA"


def test_invalid_cors_configuration_fails_startup(tmp_path):
    env = os.environ.copy()
    env.update(
        {
            "PORT": "0",
            "DB_PATH": str(tmp_path / "invalid-cors.db"),
            "CORS_ORIGINS": "httpx://invalid.example",
        }
    )
    result = subprocess.run(
        [SERVER], cwd=ROOT, env=env, capture_output=True, text=True, timeout=5
    )
    assert result.returncode != 0
    assert "CORS_ORIGINS" in result.stderr


def test_rate_limit_and_prometheus_metrics(tmp_path):
    with running_server(
        tmp_path, {"RATE_LIMIT_RPS": "1", "RATE_LIMIT_BURST": "1"}
    ) as (base_url, _):
        time.sleep(1.1)
        first_status, _, _ = http_request(base_url, "/healthz")
        second_status, second, headers = request_json(base_url, "/healthz")
        assert first_status == 200
        assert second_status == 429 and second["error"]["code"] == "RATE_LIMITED"
        assert headers["Retry-After"] == "1"
        time.sleep(1.1)
        metrics_status, metrics, metrics_headers = http_request(base_url, "/metrics")
        assert metrics_status == 200
        assert b"# TYPE reldb_buffer_pool_hits_total counter" in metrics
        assert metrics_headers["Content-Type"].startswith("text/plain; version=0.0.4")
        families = list(text_string_to_metric_families(metrics.decode()))
        assert any(family.name == "reldb_http_requests" for family in families)
        assert any(family.name == "reldb_statement_duration_us" for family in families)

def test_demo_load_is_opt_in_and_uses_statement_counters(tmp_path):
    with running_server(tmp_path, {"DEMO_LOAD": "1"}) as (base_url, _):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            status, stats, _ = request_json(base_url, "/api/v1/stats")
            if status == 200 and stats["statements"]["select"] > 0:
                break
            time.sleep(0.1)
        assert status == 200
        assert stats["demo_load"] is True
        assert stats["statements"]["select"] > 0

def test_slow_query_log_includes_executed_plan(tmp_path):
    with running_server(
        tmp_path, {"SLOW_QUERY_MS": "1", "MAX_SQL_BYTES": "60000"}
    ) as (base_url, process):
        assert post_sql(
            base_url, "CREATE TABLE slow_rows (id INT PRIMARY KEY, value INT)"
        )[0] == 200
        values = ", ".join(f"({row_id}, {row_id})" for row_id in range(3000))
        assert post_sql(base_url, f"INSERT INTO slow_rows VALUES {values}")[0] == 200
        status, result, _ = post_sql(
            base_url, "SELECT id FROM slow_rows WHERE value = -1"
        )
        assert status == 200 and result["row_count"] == 0
    log_output = process.stderr.read().decode()
    assert '"event":"slow_query"' in log_output
    assert '"statement":"SELECT"' in log_output
    assert "SeqScan" in log_output


def test_legacy_aliases_keep_deprecated_shapes(api_server):
    health_status, health, _ = request_json(api_server, "/health")
    metrics_status, metrics, _ = request_json(api_server, "/api/metrics")
    insert_status, insert, _ = request_json(
        api_server, "/api/query", {"query": "INSERT 12 120"}
    )
    query_status, query, _ = request_json(
        api_server, "/api/query", {"query": "SELECT 12"}
    )
    missing_key_status, missing_key, _ = request_json(
        api_server, "/api/query", {"query": "SELECT 99999"}
    )
    metrics_after_missing_key_status, metrics_after_missing_key, _ = request_json(
        api_server, "/api/metrics"
    )

    assert health_status == 200 and health["status"] == "ok" and health["deprecated"]
    assert metrics_status == 200 and metrics["deprecated"]
    assert insert_status == 200 and insert["result"] == "Inserted (12, 120)"
    assert query_status == 200
    assert query["status"] == "success" and query["deprecated"] is True
    assert "result" in query
    assert missing_key_status == 200 and "not found" in missing_key["result"]
    assert metrics_after_missing_key_status == 200
    assert metrics_after_missing_key["error_count"] == metrics["error_count"]
    assert metrics_after_missing_key["active_latches"] == metrics_after_missing_key["active_statements"]


def test_openapi_spec_and_query_example(api_server):
    spec_path = os.path.join(ROOT, "docs", "openapi.yaml")
    with open(spec_path, encoding="utf-8") as source:
        spec = yaml.safe_load(source)
    validate(spec)

    assert post_sql(api_server, "CREATE TABLE documented (id INT PRIMARY KEY)")[0] == 200
    assert post_sql(api_server, "INSERT INTO documented VALUES (31)")[0] == 200
    example = spec["paths"]["/api/v1/query"]["post"]["requestBody"]["content"][
        "application/json"
    ]["examples"]["parameterizedSelect"]["value"]
    status, result, _ = request_json(api_server, "/api/v1/query", example)
    assert status == 200 and result["rows"] == [[31]]
    legacy_example = spec["paths"]["/api/v1/query"]["post"]["requestBody"]["content"][
        "application/json"
    ]["examples"]["legacyQueryField"]["value"]
    legacy_status, legacy_result, _ = request_json(
        api_server, "/api/v1/query", legacy_example
    )
    assert legacy_status == 200 and legacy_result["rows"] == [[31]]


def test_recovery_readiness_after_hard_kill(tmp_path):
    database = tmp_path / "crash-recovery.db"
    with running_server(tmp_path, db_path=database) as (base_url, process):
        assert post_sql(base_url, "CREATE TABLE recovery_rows (id INT PRIMARY KEY)")[0] == 200
        assert post_sql(base_url, "INSERT INTO recovery_rows VALUES (99)")[0] == 200
        os.kill(process.pid, signal.SIGKILL)
        process.wait(timeout=5)

    with running_server(tmp_path, db_path=database) as (restarted_url, _):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            status, ready, _ = request_json(restarted_url, "/readyz")
            if status == 200:
                break
            time.sleep(0.05)
        assert status == 200 and ready["ok"] is True
        result_status, result, _ = post_sql(
            restarted_url, "SELECT id FROM recovery_rows WHERE id = 99"
        )
        assert result_status == 200 and result["rows"] == [[99]]
