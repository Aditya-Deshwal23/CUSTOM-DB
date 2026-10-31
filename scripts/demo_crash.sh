#!/usr/bin/env bash
set -euo pipefail

IMAGE="${IMAGE:-custom-db:local}"
HOST_PORT="${HOST_PORT:-18080}"
VOLUME="${VOLUME:-custom-db-durability}"
CRASH_AFTER_SECONDS="${CRASH_AFTER_SECONDS:-2}"
CONTAINER_PREFIX="custom-db-crash-$$"
BEFORE_CONTAINER="${CONTAINER_PREFIX}-before"
AFTER_CONTAINER="${CONTAINER_PREFIX}-after"
TABLE="crash_demo_$$"
BASE_URL="http://127.0.0.1:${HOST_PORT}"
WRITER_LOG="$(mktemp)"
WRITER_PID=""

cleanup() {
    if [[ -n "${WRITER_PID}" ]]; then
        kill "${WRITER_PID}" 2>/dev/null || true
        wait "${WRITER_PID}" 2>/dev/null || true
    fi
    if docker inspect "${BEFORE_CONTAINER}" >/dev/null 2>&1; then
        docker rm -f "${BEFORE_CONTAINER}" >/dev/null
    fi
    if docker inspect "${AFTER_CONTAINER}" >/dev/null 2>&1; then
        docker rm -f "${AFTER_CONTAINER}" >/dev/null
    fi
    rm -f "${WRITER_LOG}"
}
trap cleanup EXIT

IMAGE_USER="$(docker image inspect --format '{{.Config.User}}' "${IMAGE}")"
if [[ "${IMAGE_USER}" != "10001:10001" ]]; then
    echo "Expected image user 10001:10001, got '${IMAGE_USER}'" >&2
    exit 1
fi

wait_ready() {
    local attempts=0
    until python3 - "${BASE_URL}" <<'PY'
import sys
import urllib.error
import urllib.request

try:
    with urllib.request.urlopen(sys.argv[1] + "/readyz", timeout=1) as response:
        sys.exit(0 if response.status == 200 else 1)
except (OSError, urllib.error.URLError):
    sys.exit(1)
PY
    do
        attempts=$((attempts + 1))
        if [[ "${attempts}" -ge 60 ]]; then
            echo "Timed out waiting for ${BASE_URL}/readyz" >&2
            return 1
        fi
        sleep 0.25
    done
}

post_sql() {
    python3 - "$BASE_URL" "$1" <<'PY'
import json
import sys
import urllib.request

request = urllib.request.Request(
    sys.argv[1] + "/api/v1/query",
    data=json.dumps({"sql": sys.argv[2]}).encode(),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=5) as response:
    payload = json.load(response)
if not payload.get("ok"):
    raise SystemExit(json.dumps(payload))
print(json.dumps(payload))
PY
}

docker volume create "${VOLUME}" >/dev/null
docker run --detach \
    --name "${BEFORE_CONTAINER}" \
    --read-only \
    --tmpfs /tmp \
    --cap-drop ALL \
    --security-opt no-new-privileges:true \
    --security-opt no-new-privileges:true \
    --mount "type=volume,source=${VOLUME},target=/data" \
    --publish "${HOST_PORT}:8080" \
    --env PORT=8080 \
    --env DB_PATH=/data/custom.db \
    --env READ_ONLY=0 \
    --env DURABILITY=full \
    --env RATE_LIMIT_RPS=1000 \
    --env RATE_LIMIT_BURST=2000 \
    "${IMAGE}" >/dev/null
wait_ready
post_sql "CREATE TABLE ${TABLE} (id INT PRIMARY KEY, payload INT)" >/dev/null

python3 -u - "${BASE_URL}" "${TABLE}" >"${WRITER_LOG}" 2>&1 <<'PY' &
import json
import sys
import time
import urllib.error
import urllib.request

base_url, table = sys.argv[1:3]
acknowledged = 0
while True:
    sql = f"INSERT INTO {table} (id, payload) VALUES ({acknowledged + 1}, 1)"
    request = urllib.request.Request(
        base_url + "/api/v1/query",
        data=json.dumps({"sql": sql}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=2) as response:
            response.read()
        acknowledged += 1
    except (OSError, urllib.error.URLError) as error:
        print(json.dumps({"acknowledged": acknowledged, "writer_stopped": str(error)}))
        break
PY
WRITER_PID=$!
sleep "${CRASH_AFTER_SECONDS}"
echo "Hard-killing ${BEFORE_CONTAINER} during the write loop"
docker kill --signal KILL "${BEFORE_CONTAINER}" >/dev/null
wait "${WRITER_PID}" 2>/dev/null || true
WRITER_PID=""
cat "${WRITER_LOG}"
ACKNOWLEDGED_WRITES="$(python3 - "${WRITER_LOG}" <<'PY'
import json
import pathlib
import sys

lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
if not lines:
    raise SystemExit("Writer did not report its acknowledged write count")
try:
    report = json.loads(lines[-1])
except json.JSONDecodeError as error:
    raise SystemExit(f"Writer report is not valid JSON: {error}") from error
if "acknowledged" not in report:
    raise SystemExit("Writer report is missing the acknowledged write count")
print(int(report["acknowledged"]))
PY
)"
docker rm "${BEFORE_CONTAINER}" >/dev/null

docker run --detach \
    --name "${AFTER_CONTAINER}" \
    --read-only \
    --tmpfs /tmp \
    --cap-drop ALL \
    --security-opt no-new-privileges:true \
    --security-opt no-new-privileges:true \
    --mount "type=volume,source=${VOLUME},target=/data" \
    --publish "${HOST_PORT}:8080" \
    --env PORT=8080 \
    --env DB_PATH=/data/custom.db \
    --env READ_ONLY=0 \
    --env DURABILITY=full \
    --env RATE_LIMIT_RPS=1000 \
    --env RATE_LIMIT_BURST=2000 \
    "${IMAGE}" >/dev/null
wait_ready

echo "Recovery log:"
docker logs "${AFTER_CONTAINER}"
echo "Integrity check:"
docker exec "${AFTER_CONTAINER}" /app/relational_db --check
echo "Recovered rows:"
RECOVERED_ROWS="$(post_sql "SELECT COUNT(*) FROM ${TABLE}")"
printf '%s\n' "${RECOVERED_ROWS}"
python3 - "${RECOVERED_ROWS}" <<'PY'
import json
import sys

result = json.loads(sys.argv[1])
if not result.get("ok") or not result.get("rows") or int(result["rows"][0][0]) < 1:
    raise SystemExit("No committed rows were recovered after the hard kill")
PY
python3 - "${RECOVERED_ROWS}" "${ACKNOWLEDGED_WRITES}" <<'PY'
import json
import sys

result = json.loads(sys.argv[1])
recovered = int(result["rows"][0][0])
acknowledged = int(sys.argv[2])
if recovered < acknowledged:
    raise SystemExit(
        f"Recovered {recovered} rows, fewer than {acknowledged} acknowledged writes"
    )
print(f"Verified {acknowledged} acknowledged writes; recovered {recovered} rows.")
PY

docker stop --time 10 "${AFTER_CONTAINER}" >/dev/null
EXIT_CODE="$(docker inspect --format '{{.State.ExitCode}}' "${AFTER_CONTAINER}")"
if [[ "${EXIT_CODE}" != "0" ]]; then
    echo "Graceful stop returned ${EXIT_CODE}, expected 0" >&2
    docker logs "${AFTER_CONTAINER}" >&2
    exit 1
fi
STOP_LOG="$(docker logs "${AFTER_CONTAINER}" 2>&1)"
if [[ "${STOP_LOG}" != *'"event":"drain_complete"'* ||
      "${STOP_LOG}" != *'"event":"shutdown_checkpoint_complete"'* ]]; then
    echo "Graceful-stop drain/checkpoint log entries were not found" >&2
    printf '%s\n' "${STOP_LOG}" >&2
    exit 1
fi
echo "Graceful stop exited 0 with drain and checkpoint logs."
