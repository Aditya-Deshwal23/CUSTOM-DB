#!/usr/bin/env bash
set -euo pipefail

IMAGE="${IMAGE:-ghcr.io/aditya-deshwal23/custom-db:1.0.0}"
HOST_PORT="${HOST_PORT:-18080}"
VOLUME="custom-db-quickstart-$$"
CONTAINER="custom-db-quickstart-$$"
TABLE="quickstart_$$"
BASE_URL="http://127.0.0.1:${HOST_PORT}"

cleanup() {
    docker rm -f "${CONTAINER}" >/dev/null 2>&1 || true
    docker volume rm "${VOLUME}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker volume create "${VOLUME}" >/dev/null
docker run --detach \
    --name "${CONTAINER}" \
    --publish "${HOST_PORT}:8080" \
    --mount "type=volume,source=${VOLUME},target=/data" \
    --env PORT=8080 \
    --env DB_PATH=/data/custom.db \
    "${IMAGE}" >/dev/null

attempt=0
until curl --fail --silent --show-error "${BASE_URL}/readyz" >/dev/null; do
    attempt=$((attempt + 1))
    if [[ "${attempt}" -ge 60 ]]; then
        echo "Timed out waiting for ${BASE_URL}/readyz" >&2
        docker logs "${CONTAINER}" >&2
        exit 1
    fi
    sleep 0.25
done

curl --fail --silent --show-error \
    -H 'Content-Type: application/json' \
    -d "{\"sql\":\"CREATE TABLE ${TABLE} (id INT PRIMARY KEY, value INT)\"}" \
    "${BASE_URL}/api/v1/query"
curl --fail --silent --show-error \
    -H 'Content-Type: application/json' \
    -d "{\"sql\":\"INSERT INTO ${TABLE} VALUES (1, 42)\"}" \
    "${BASE_URL}/api/v1/query"
result="$(curl --fail --silent --show-error \
    -H 'Content-Type: application/json' \
    -d "{\"sql\":\"SELECT id, value FROM ${TABLE}\"}" \
    "${BASE_URL}/api/v1/query")"
printf '%s\n' "${result}"
python3 -c 'import json,sys; data=json.loads(sys.argv[1]); assert data["rows"] == [[1,42]], data' "${result}"
echo "Quickstart passed against ${IMAGE}."
