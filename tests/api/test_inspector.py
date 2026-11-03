import base64
import json
import random
import struct
import time
from urllib.parse import quote

import pytest

from test_api import http_request, post_sql, request_json, running_server


@pytest.fixture
def api_server(tmp_path):
    with running_server(tmp_path) as server:
        yield server[0]


def crc32c(data):
    checksum = 0xFFFFFFFF
    for byte in data:
        checksum ^= byte
        for _ in range(8):
            checksum = (checksum >> 1) ^ (0x82F63B78 if checksum & 1 else 0)
    return checksum ^ 0xFFFFFFFF


def test_snapshot_is_single_cached_source(api_server):
    status, snapshot, headers = request_json(api_server, "/api/v1/snapshot")

    assert status == 200
    assert {
        "ts_ms",
        "engine",
        "stats",
        "storage",
        "pool",
        "wal",
        "activity",
    } <= snapshot.keys()
    assert {"version", "git_sha", "format_version", "pool_frames"} <= snapshot["engine"].keys()
    assert {"size", "pinned", "dirty", "free", "hit_ratio_window"} <= snapshot["pool"].keys()
    assert headers["ETag"]
    _, compact_pool, _ = request_json(api_server, "/api/v1/stats/buffer_pool")
    frame_states = base64.b64decode(compact_pool["states_base64"])
    assert snapshot["pool"]["free"] == snapshot["pool"]["size"] - sum(
        state & 1 for state in frame_states
    )

    cached_status, cached_body, cached_headers = http_request(
        api_server,
        "/api/v1/snapshot",
        headers={"If-None-Match": headers["ETag"]},
    )
    assert cached_status == 304 and cached_body == b""
    assert cached_headers["ETag"] == headers["ETag"]


def test_buffer_pool_compact_format_and_legacy_compatibility(api_server):
    compact_status, compact, _ = request_json(api_server, "/api/v1/stats/buffer_pool")
    legacy_status, legacy, _ = request_json(
        api_server, "/api/v1/stats/buffer_pool?format=legacy"
    )

    assert compact_status == 200
    assert compact["encoding"] == "base64-state-v1"
    assert len(base64.b64decode(compact["states_base64"])) == compact["snapshot_size"]
    assert len(json.dumps(compact)) <= 20 * 1024
    assert legacy_status == 200 and isinstance(legacy["frames"], list)


def test_catalog_is_returned_in_one_bounded_response(api_server):
    assert post_sql(api_server, "CREATE TABLE inspector_catalog (id INT PRIMARY KEY)")[0] == 200
    status, catalog, _ = request_json(api_server, "/api/v1/catalog")

    assert status == 200
    assert len(catalog["tables"]) == 1
    table = catalog["tables"][0]
    assert table["name"] == "inspector_catalog"
    assert {"columns", "indexes", "heap", "row_estimate", "ddl"} <= table.keys()
    assert table["columns"][0]["pk"] is True
    assert "CREATE TABLE inspector_catalog" in table["ddl"]

    before_status, before, _ = request_json(
        api_server, "/api/v1/stats/buffer_pool?format=legacy"
    )
    after_catalog_status, _, _ = request_json(api_server, "/api/v1/catalog")
    after_status, after, _ = request_json(
        api_server, "/api/v1/stats/buffer_pool?format=legacy"
    )
    assert before_status == after_status == after_catalog_status == 200
    assert before["frames"] == after["frames"]


def test_inspector_authentication_and_disable_switch(tmp_path):
    with running_server(
        tmp_path, {"API_KEY": "inspection-secret"}, db_path=tmp_path / "auth.db"
    ) as (base_url, _):
        assert http_request(base_url, "/api/v1/snapshot")[0] == 401
        assert http_request(
            base_url,
            "/api/v1/snapshot",
            headers={"X-API-Key": "inspection-secret"},
        )[0] == 200

    with running_server(
        tmp_path, {"INSPECT_ENABLED": "0"}, db_path=tmp_path / "disabled.db"
    ) as (base_url, _):
        assert http_request(base_url, "/api/v1/snapshot")[0] == 404
        assert http_request(base_url, "/api/v1/stats")[0] == 200


def test_inspector_rate_limit_has_separate_bucket(api_server):
    statuses = [http_request(api_server, "/api/v1/snapshot")[0] for _ in range(11)]
    assert statuses.count(429) == 1
    assert statuses[-1] == 429


def test_page_map_and_defensive_page_decoder(api_server):
    assert post_sql(api_server, "CREATE TABLE page_inspector (id INT PRIMARY KEY)")[0] == 200
    assert post_sql(api_server, "INSERT INTO page_inspector VALUES (41), (42)")[0] == 200
    status, page_map, _ = request_json(api_server, "/api/v1/inspect/pages?from=0&count=4096")
    assert status == 200 and page_map["total"] > 1
    assert page_map["count"] == page_map["total"]
    page_types = base64.b64decode(page_map["page_types_base64"])
    assert len(page_types) == page_map["total"]
    assert page_types[0] == 0

    header_status, header, _ = request_json(api_server, "/api/v1/inspect/pages/0")
    assert header_status == 200 and header["type"] == "HEADER"
    assert header["crc_ok"] is True
    assert header["decoded"]["page_size"] == 4096
    assert header["decoded"]["page_count"] == page_map["total"]

    rows_status, rows, _ = request_json(
        api_server, "/api/v1/tables/page_inspector/rows?limit=10&order=id"
    )
    assert rows_status == 200
    data_page = int(rows["rows"][0]["rid"].split(":", 1)[0])
    data_status, data, _ = request_json(api_server, f"/api/v1/inspect/pages/{data_page}")
    assert data_status == 200 and data["crc_ok"] is True
    assert data["decoded"]["table"] == "page_inspector"
    assert data["decoded"]["tuples"]
    assert {
        row["rid"]: row["values"] for row in data["decoded"]["tuples"]
    } == {
        row["rid"]: row["values"]
        for row in rows["rows"]
        if int(row["rid"].split(":", 1)[0]) == data_page
    }

    for path in (
        "/api/v1/inspect/pages?from=-1",
        "/api/v1/inspect/pages?count=4097",
        "/api/v1/inspect/pages?count=not-a-number",
        "/api/v1/inspect/pages/-1",
        "/api/v1/inspect/pages/0?raw=2",
    ):
        assert http_request(api_server, path)[0] in (400, 422)
    assert http_request(api_server, "/api/v1/inspect/pages/0?raw=1")[0] == 403


def test_page_decoder_covers_persistent_page_types(tmp_path):
    with running_server(tmp_path, db_path=tmp_path / "page-types.db") as (base_url, _):
        assert post_sql(base_url, "CREATE TABLE page_types (id INT PRIMARY KEY)")[0] == 200
        values = ", ".join(f"({value})" for value in range(220))
        assert post_sql(base_url, f"INSERT INTO page_types VALUES {values}")[0] == 200
        assert post_sql(base_url, "CREATE TABLE dropped_pages (id INT PRIMARY KEY)")[0] == 200
        assert post_sql(base_url, "DROP TABLE dropped_pages")[0] == 200

        map_status, page_map, _ = request_json(
            base_url, "/api/v1/inspect/pages?from=0&count=4096"
        )
        assert map_status == 200
        page_types = base64.b64decode(page_map["page_types_base64"])
        representative_pages = {}
        for page_id, page_type in enumerate(page_types):
            representative_pages.setdefault(page_type, page_id)

        expected_types = set(range(7))
        assert expected_types <= representative_pages.keys()
        rows_status, rows, _ = request_json(
            base_url, "/api/v1/tables/page_types/rows?limit=1&order=id"
        )
        assert rows_status == 200
        heap_data_page = int(rows["rows"][0]["rid"].split(":", 1)[0])
        for page_type in sorted(expected_types):
            page_id = (
                heap_data_page
                if page_type == 4
                else representative_pages[page_type]
            )
            status, page, _ = request_json(
                base_url, f"/api/v1/inspect/pages/{page_id}"
            )
            assert status == 200
            assert page["crc_ok"] is True
            assert "decode_error" not in page["decoded"]


def test_corrupt_page_decode_reports_error_and_keeps_server_alive(tmp_path):
    database_path = tmp_path / "corrupt-page.db"
    with running_server(tmp_path, db_path=database_path) as (base_url, process):
        assert post_sql(base_url, "CREATE TABLE corrupt_page (id INT PRIMARY KEY)")[0] == 200
        assert post_sql(base_url, "INSERT INTO corrupt_page VALUES (1)")[0] == 200
        rows_status, rows, _ = request_json(
            base_url, "/api/v1/tables/corrupt_page/rows?limit=1&order=id"
        )
        assert rows_status == 200
        page_id = int(rows["rows"][0]["rid"].split(":", 1)[0])
        process.terminate()
        process.wait(timeout=5)

    with database_path.open("r+b") as database:
        page_offset = page_id * 4096
        database.seek(page_offset)
        original_page = database.read(4096)
        damaged_page = bytearray(original_page)
        damaged_page[0] ^= 1
        database.seek(page_offset)
        database.write(damaged_page)
        database.flush()

    with running_server(tmp_path, db_path=database_path) as (base_url, _):
        status, page, _ = request_json(base_url, f"/api/v1/inspect/pages/{page_id}")
        assert status == 200
        assert page["crc_ok"] is False
        assert page["decoded"]["decode_error"]
        assert http_request(base_url, "/healthz")[0] == 200


def test_random_page_bytes_with_valid_checksum_are_rejected_safely(tmp_path):
    database_path = tmp_path / "random-page.db"
    with running_server(tmp_path, db_path=database_path) as (base_url, process):
        assert post_sql(base_url, "CREATE TABLE random_page (id INT PRIMARY KEY)")[0] == 200
        assert post_sql(base_url, "INSERT INTO random_page VALUES (1)")[0] == 200
        rows_status, rows, _ = request_json(
            base_url, "/api/v1/tables/random_page/rows?limit=1&order=id"
        )
        assert rows_status == 200
        page_id = int(rows["rows"][0]["rid"].split(":", 1)[0])
        process.terminate()
        process.wait(timeout=5)

    with database_path.open("r+b") as database:
        page_offset = page_id * 4096
        database.seek(page_offset)
        physical_page = bytearray(database.read(4096))
        generator = random.Random(731291)
        physical_page[16:] = generator.randbytes(4080)
        physical_page[:4] = crc32c(physical_page[4:]).to_bytes(4, "little")
        database.seek(page_offset)
        database.write(physical_page)
        database.flush()

    with running_server(tmp_path, db_path=database_path) as (base_url, _):
        status, page, _ = request_json(base_url, f"/api/v1/inspect/pages/{page_id}")
        assert status == 200
        assert page["crc_ok"] is True
        assert page["decoded"]["decode_error"]
        assert http_request(base_url, "/healthz")[0] == 200


def test_index_inspection_decodes_integer_keys(api_server):
    assert post_sql(api_server, "CREATE TABLE decoded_index (id INT PRIMARY KEY)")[0] == 200
    assert post_sql(api_server, "INSERT INTO decoded_index VALUES (41), (42)")[0] == 200
    catalog_status, catalog, _ = request_json(api_server, "/api/v1/catalog")
    assert catalog_status == 200
    index_name = catalog["tables"][0]["indexes"][0]["name"]

    tree_status, tree, _ = request_json(
        api_server, f"/api/v1/inspect/indexes/{quote(index_name, safe='')}/tree"
    )
    assert tree_status == 200
    assert tree["nodes"][0]["min_key"] == 41
    assert tree["nodes"][0]["max_key"] == 42

    leaf = next(node for node in tree["nodes"] if not node["children"])
    page_status, page, _ = request_json(
        api_server, f"/api/v1/inspect/pages/{leaf['page_id']}"
    )
    assert page_status == 200
    assert [entry["key"] for entry in page["decoded"]["entries"]] == [41, 42]


def test_index_inspection_decodes_each_supported_key_kind(api_server):
    cases = (
        ("BIGINT", ["9000000000", "9000000001"], [9000000000, 9000000001]),
        ("DOUBLE", ["1.5", "2.5"], [1.5, 2.5]),
        ("BOOLEAN", ["false", "true"], [False, True]),
        ("VARCHAR(16)", ["'apple'", "'zebra'"], ["apple", "zebra"]),
    )
    for case_number, (sql_type, literals, values) in enumerate(cases):
        time.sleep(1.1)
        table_name = f"decoded_key_{case_number}"
        assert post_sql(
            api_server,
            f"CREATE TABLE {table_name} (id {sql_type} PRIMARY KEY)",
        )[0] == 200
        assert post_sql(
            api_server,
            f"INSERT INTO {table_name} VALUES "
            + ", ".join(f"({literal})" for literal in literals),
        )[0] == 200

        catalog_status, catalog, _ = request_json(api_server, "/api/v1/catalog")
        assert catalog_status == 200
        table = next(item for item in catalog["tables"] if item["name"] == table_name)
        index_name = table["indexes"][0]["name"]
        tree_status, tree, _ = request_json(
            api_server, f"/api/v1/inspect/indexes/{quote(index_name, safe='')}/tree"
        )
        assert tree_status == 200
        leaf = next(node for node in tree["nodes"] if not node["children"])
        page_status, page, _ = request_json(
            api_server, f"/api/v1/inspect/pages/{leaf['page_id']}"
        )
        assert page_status == 200
        decoded_keys = [entry["key"] for entry in page["decoded"]["entries"]]

        if sql_type == "VARCHAR(16)":
            def fnv_hash(text):
                result = 14695981039346656037
                for byte in text.encode("ascii"):
                    result = ((result ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
                return base64.b64encode(result.to_bytes(8, "big")).decode("ascii")

            expected_keys = [
                (fnv_hash(value), value) for value in values
            ]
            expected_keys.sort(key=lambda item: base64.b64decode(item[0]))
            expected_keys = [{"hash_base64": digest} for digest, _ in expected_keys]
            assert decoded_keys == expected_keys
        else:
            assert decoded_keys == values
        assert tree["nodes"][0]["min_key"] == decoded_keys[0]
        assert tree["nodes"][0]["max_key"] == decoded_keys[-1]


def test_full_page_scan_does_not_change_buffer_pool_residency(api_server):
    before_status, before, _ = request_json(
        api_server, "/api/v1/stats/buffer_pool?format=legacy"
    )
    inspect_status, page_map, _ = request_json(
        api_server, "/api/v1/inspect/pages?from=0&count=4096"
    )
    after_status, after, _ = request_json(
        api_server, "/api/v1/stats/buffer_pool?format=legacy"
    )

    assert before_status == inspect_status == after_status == 200
    assert page_map["count"] == page_map["total"]
    assert before["frames"] == after["frames"]


def test_raw_page_bytes_are_explicitly_config_gated(tmp_path):
    with running_server(
        tmp_path, {"INSPECT_RAW_BYTES": "1"}, db_path=tmp_path / "raw-enabled.db"
    ) as (base_url, _):
        status, page, _ = request_json(base_url, "/api/v1/inspect/pages/0?raw=1")
        assert status == 200
        assert len(base64.b64decode(page["hex_base64"])) == 4096


def test_wal_and_freelist_inspection_are_bounded(api_server):
    assert post_sql(api_server, "CREATE TABLE inspector_wal (id INT PRIMARY KEY)")[0] == 200
    status, wal, _ = request_json(api_server, "/api/v1/inspect/wal?limit=50")

    assert status == 200
    assert wal["size_bytes"] > 0
    assert wal["base_lsn"] >= 0
    assert wal["truncated"] is False
    assert len(wal["records"]) <= 50
    assert {"lsn", "type", "txn_id", "page_id", "payload_bytes", "crc_ok", "file_offset"} <= (
        wal["records"][-1].keys()
    )
    assert any(record["type"] == "COMMIT" for record in wal["records"])
    assert wal["commit_lsns"]

    last_lsn = max(record["lsn"] for record in wal["records"])
    older_status, older, _ = request_json(
        api_server, f"/api/v1/inspect/wal?limit=50&before_lsn={last_lsn}"
    )
    assert older_status == 200
    assert all(record["lsn"] < last_lsn for record in older["records"])
    assert http_request(api_server, "/api/v1/inspect/wal?limit=501")[0] == 400
    assert http_request(api_server, "/api/v1/inspect/wal?before_lsn=-1")[0] == 400

    freelist_status, freelist, _ = request_json(api_server, "/api/v1/inspect/freelist")
    assert freelist_status == 200
    assert isinstance(freelist["pages"], list)
    assert freelist["truncated"] is False


def test_wal_inspection_exposes_a_bad_record_checksum(tmp_path):
    database_path = tmp_path / "wal-checksum.db"
    with running_server(tmp_path, db_path=database_path) as (base_url, _):
        assert post_sql(base_url, "CREATE TABLE wal_checksum (id INT)")[0] == 200
        status, wal, _ = request_json(base_url, "/api/v1/inspect/wal?limit=50")
        assert status == 200 and wal["records"]
        record = wal["records"][-1]
        assert record["type"] == "COMMIT" and record["crc_ok"] is True

        wal_path = database_path.with_name(database_path.name + "-wal")
        with wal_path.open("r+b") as source:
            source.seek(record["file_offset"])
            record_length = struct.unpack("<I", source.read(4))[0]
            source.seek(record["file_offset"] + record_length - 1)
            byte = source.read(1)
            source.seek(record["file_offset"] + record_length - 1)
            source.write(bytes([byte[0] ^ 1]))

        damaged_status, damaged, _ = request_json(
            base_url, "/api/v1/inspect/wal?limit=50"
        )
        assert damaged_status == 200
        assert damaged["truncated"] is True
        assert damaged["records"][-1]["lsn"] == record["lsn"]
        assert damaged["records"][-1]["crc_ok"] is False


def test_index_tree_and_lookup_trace_use_existing_index(api_server):
    assert post_sql(api_server, "CREATE TABLE inspector_index (id INT PRIMARY KEY)")[0] == 200
    assert post_sql(api_server, "CREATE INDEX inspector_index_lookup ON inspector_index (id)")[0] == 200
    values = ", ".join(f"({value})" for value in range(220))
    assert post_sql(api_server, f"INSERT INTO inspector_index VALUES {values}")[0] == 200

    tree_status, tree, _ = request_json(
        api_server, "/api/v1/inspect/indexes/inspector_index_lookup/tree?max_nodes=20"
    )
    assert tree_status == 200
    assert len(tree["nodes"]) >= 3 and tree["truncated"] is False
    assert tree["nodes"][0]["level"] == 2
    leaves = [node for node in tree["nodes"] if node["level"] == 1]
    assert sum(node["count"] for node in leaves) == 220
    decoded_entries = []
    for leaf in sorted(leaves, key=lambda node: node["min_key"]):
        page_status, page, _ = request_json(
            api_server, f"/api/v1/inspect/pages/{leaf['page_id']}"
        )
        assert page_status == 200
        decoded_entries.extend(
            (leaf["page_id"], entry) for entry in page["decoded"]["entries"]
        )
    assert [entry["key"] for _, entry in decoded_entries] == list(range(220))

    trace_status, trace, _ = request_json(
        api_server, "/api/v1/inspect/indexes/inspector_index_lookup/trace?key=2"
    )
    assert trace_status == 200
    assert trace["found"] is True
    assert trace["rid"]
    assert len(trace["path"]) == 2
    assert trace["path"][-1]["page_id"] in {node["page_id"] for node in leaves}
    traced_leaf = next(
        (page_id, entry)
        for page_id, entry in decoded_entries
        if entry["key"] == 2
        and f'{entry["rid"] >> 16}:{entry["rid"] & 0xFFFF}' == trace["rid"]
    )
    assert traced_leaf[1]["key"] == 2
    assert traced_leaf[0] == trace["path"][-1]["page_id"]
    assert http_request(
        api_server, "/api/v1/inspect/indexes/inspector_index_lookup/tree?max_nodes=201"
    )[0] == 400
    assert http_request(
        api_server, "/api/v1/inspect/indexes/inspector_index_lookup/trace?key=not-an-int"
    )[0] == 400


def test_table_rows_use_typed_keyset_pagination_and_filters(api_server):
    assert post_sql(
        api_server,
        "CREATE TABLE inspector_rows (id INT PRIMARY KEY, label VARCHAR(32), active BOOLEAN)",
    )[0] == 200
    assert post_sql(
        api_server,
        "INSERT INTO inspector_rows VALUES "
        "(1, 'one', TRUE), (2, 'two', FALSE), (3, 'three', TRUE), "
        "(4, 'four', TRUE), (5, 'three', FALSE)",
    )[0] == 200

    status, first, _ = request_json(
        api_server, "/api/v1/tables/inspector_rows/rows?limit=2&order=id"
    )
    assert status == 200
    assert [row["values"]["id"] for row in first["rows"]] == [1, 2]
    assert all(":" in row["rid"] for row in first["rows"])
    assert first["next_cursor"]

    status, second, _ = request_json(
        api_server,
        f"/api/v1/tables/inspector_rows/rows?limit=2&order=id&after={quote(first['next_cursor'])}",
    )
    assert status == 200
    assert [row["values"]["id"] for row in second["rows"]] == [3, 4]
    assert second["next_cursor"]
    status, third, _ = request_json(
        api_server,
        f"/api/v1/tables/inspector_rows/rows?limit=2&order=id&after={quote(second['next_cursor'])}",
    )
    assert status == 200
    assert [row["values"]["id"] for row in third["rows"]] == [5]
    assert third["next_cursor"] is None

    filter_spec = quote(json.dumps({"column": "active", "op": "=", "value": True}))
    filtered_status, filtered, _ = request_json(
        api_server,
        f"/api/v1/tables/inspector_rows/rows?limit=10&order=id&filter={filter_spec}",
    )
    assert filtered_status == 200
    assert [row["values"]["id"] for row in filtered["rows"]] == [1, 3, 4]
    descending_status, descending, _ = request_json(
        api_server, "/api/v1/tables/inspector_rows/rows?limit=2&order=label:desc"
    )
    assert descending_status == 200
    assert [row["values"]["id"] for row in descending["rows"]] == [2, 3]
    next_desc_status, next_desc, _ = request_json(
        api_server,
        f"/api/v1/tables/inspector_rows/rows?limit=2&order=label:desc"
        f"&after={quote(descending['next_cursor'])}",
    )
    assert next_desc_status == 200
    assert [row["values"]["id"] for row in next_desc["rows"]] == [5, 1]
    assert http_request(
        api_server, "/api/v1/tables/inspector_rows/rows?limit=501"
    )[0] == 400
    assert http_request(
        api_server, "/api/v1/tables/inspector_rows/rows?after=invalid"
    )[0] == 400


def test_query_reports_statement_local_page_touches_and_write_commit(api_server):
    create_status, create, _ = post_sql(
        api_server, "CREATE TABLE inspector_commit (id INT PRIMARY KEY)"
    )
    assert create_status == 200
    assert create["commit"]["lsn"] > 0
    assert create["commit"]["pages_written"] > 0
    assert create["commit"]["bytes"] > 0
    assert create["commit"]["fsync_us"] >= 0
    assert set(create["pages_touched"]) == {"hit", "miss"}

    insert_status, insert, _ = post_sql(
        api_server, "INSERT INTO inspector_commit VALUES (7)"
    )
    assert insert_status == 200
    assert insert["commit"]["pages_written"] > 0
    assert insert["pages_touched"]["hit"] >= 0
    assert insert["pages_touched"]["miss"] >= 0

    select_status, select, _ = post_sql(
        api_server, "SELECT id FROM inspector_commit WHERE id = 7"
    )
    assert select_status == 200
    assert select["rows"] == [[7]]
    assert select["pages_touched"]["hit"] + select["pages_touched"]["miss"] > 0
    assert "commit" not in select


def test_history_is_columnar_and_bounded(api_server):
    time.sleep(1.1)
    status, history, _ = request_json(
        api_server, "/api/v1/stats/history?window_s=60&step_s=1"
    )
    assert status == 200 and len(history["ts_ms"]) >= 2
    assert history["step_s"] == 1
    assert len(history["series"]) >= 40
    assert all(len(values) == len(history["ts_ms"]) for values in history["series"].values())
    assert http_request(api_server, "/api/v1/stats/history?window_s=3601")[0] == 400
    assert http_request(api_server, "/api/v1/stats/history?step_s=-1")[0] == 400


def test_slow_query_ring_omits_bound_parameter_values(tmp_path):
    with running_server(
        tmp_path, {"SLOW_QUERY_MS": "1"}, db_path=tmp_path / "slow-inspector.db"
    ) as (base_url, _):
        assert post_sql(base_url, "CREATE TABLE slow_payload (id INT, label VARCHAR(255))")[0] == 200
        for batch in range(10):
            start = batch * 40
            batch_rows = ", ".join(
                f"({index}, '{'x' * 200}')" for index in range(start, start + 40)
            )
            assert post_sql(
                base_url, f"INSERT INTO slow_payload VALUES {batch_rows}"
            )[0] == 200
        status, _, _ = request_json(
            base_url,
            "/api/v1/query",
            {"sql": "SELECT id, label FROM slow_payload WHERE id >= $1 ORDER BY label",
             "params": [0]},
        )
        assert status == 200
        slow_status, slow, _ = request_json(base_url, "/api/v1/slow-queries")
        assert slow_status == 200
        assert len(slow["queries"]) <= 100
        assert any(
            "$1" in item["sql_preview"] and "0" not in item["sql_preview"]
            for item in slow["queries"]
        )
