import os
import json
import socket
import subprocess
import time
import urllib.request

import pytest
import yaml


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
SERVER = os.environ.get(
    "RELDB_SERVER_BINARY", os.path.join(ROOT, "build-debug", "relational_db")
)


@pytest.mark.parametrize(
    ("name", "value"),
    [
        ("PORT", "0"),
        ("BUFFER_POOL_FRAMES", "15"),
        ("DURABILITY", "unsafe"),
    ],
)
def test_invalid_configuration_exits_78_with_variable_name(tmp_path, name, value):
    env = os.environ.copy()
    env.update({"DB_PATH": str(tmp_path / "invalid-config.db"), name: value})
    result = subprocess.run(
        [SERVER], cwd=ROOT, env=env, capture_output=True, text=True, timeout=5
    )
    assert result.returncode == 78
    assert name in result.stderr


def test_effective_config_is_redacted_and_sigterm_checkpoints(tmp_path):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    secret = "startup-test-secret"
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "DB_PATH": str(tmp_path / "lifecycle.db"),
            "API_KEY": secret,
            "LOG_LEVEL": "info",
        }
    )
    process = subprocess.Popen(
        [SERVER], cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                pytest.fail(f"server exited during startup: {process.stderr.read()}")
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/healthz", timeout=0.2):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            pytest.fail("server did not start")

        assert subprocess.run(
            [SERVER, "--healthcheck"], cwd=ROOT, env=env, timeout=3
        ).returncode == 0
        process.terminate()
        stdout, stderr = process.communicate(timeout=10)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)

    assert process.returncode == 0
    log_lines = [line for line in (stdout + stderr).splitlines() if line.strip()]
    records = [json.loads(line) for line in log_lines]
    config = next(record for record in records if record.get("event") == "effective_config")
    assert config["fields"]["API_KEY"] == "[redacted]"
    assert secret not in stdout + stderr
    assert any(record.get("event") == "drain_complete" for record in records)
    assert any(record.get("event") == "shutdown_checkpoint_complete" for record in records)

    checked = subprocess.run(
        [SERVER, "--check"], cwd=ROOT, env=env, capture_output=True, text=True, timeout=5
    )
    assert checked.returncode == 0
    assert json.loads(checked.stdout)["ok"] is True
    checkpointed = subprocess.run(
        [SERVER, "--checkpoint"], cwd=ROOT, env=env, capture_output=True, text=True, timeout=5
    )
    assert checkpointed.returncode == 0
    assert json.loads(checkpointed.stdout)["command"] == "checkpoint"
    version = subprocess.run(
        [SERVER, "--version"], cwd=ROOT, env=env, capture_output=True, text=True, timeout=5
    )
    assert version.returncode == 0
    assert json.loads(version.stdout)["version"]


def test_container_and_render_manifests_keep_expected_runtime_settings():
    with open(os.path.join(ROOT, "docker-compose.yml"), encoding="utf-8") as compose_file:
        compose = yaml.safe_load(compose_file)
    service = compose["services"]["custom-db"]
    assert service["read_only"] is True
    assert service["init"] is True
    assert service["cap_drop"] == ["ALL"]
    assert "no-new-privileges:true" in service["security_opt"]
    assert "/data" in service["volumes"][0]

    with open(os.path.join(ROOT, "render.yaml"), encoding="utf-8") as render_file:
        render = yaml.safe_load(render_file)
    web_service = render["services"][0]
    assert web_service["runtime"] == "docker"
    assert web_service["healthCheckPath"] == "/readyz"
    render_env = {item["key"]: item["value"] for item in web_service["envVars"]}
    assert render_env["READ_ONLY"] == "1"
