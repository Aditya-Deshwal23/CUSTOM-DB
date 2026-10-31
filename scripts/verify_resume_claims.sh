#!/usr/bin/env bash
set -euo pipefail

port="${PORT:-18080}"
server_bin="${SERVER_BIN:-./build/relational_db}"
work_dir="$(mktemp -d)"
log_file="$work_dir/server.log"
server_pid=""

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$work_dir"
}
trap cleanup EXIT

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

PORT="$port" DB_PATH="$work_dir/verify.db" "$server_bin" >"$log_file" 2>&1 &
server_pid=$!

for _ in {1..50}; do
    if curl --silent --fail "http://127.0.0.1:$port/health" |
        grep -Eq '"status"[[:space:]]*:[[:space:]]*"ok"'; then
        break
    fi
    sleep 0.1
done

curl --silent --fail "http://127.0.0.1:$port/" | grep -qi '<!doctype html>'
curl --silent --fail "http://127.0.0.1:$port/index.css" >/dev/null
curl --silent --fail "http://127.0.0.1:$port/index.js" >/dev/null
buffer_pool="$(curl --silent --fail "http://127.0.0.1:$port/api/buffer_pool")"
grep -q '"pinned_frames"' <<<"$buffer_pool"
grep -q '"frames"' <<<"$buffer_pool"
metrics="$(curl --silent --fail "http://127.0.0.1:$port/api/metrics")"
grep -q '"query_throughput"' <<<"$metrics"
grep -q '"source":"live"' <<<"$metrics"
grep -q '"cache_hit_rate"' <<<"$metrics"
grep -q '"avg_latency"' <<<"$metrics"
grep -q '"error_count"' <<<"$metrics"

query() {
    curl --silent --fail --request POST "http://127.0.0.1:$port/api/query" \
        --header 'Content-Type: application/json' \
        --data "{\"query\":\"$1\"}"
}

query 'INSERT 42 100' | grep -q 'Inserted (42, 100)'
query 'SELECT 42' | grep -q 'Result: 100'
duplicate_response="$(curl --silent --write-out $'\n%{http_code}' \
    --request POST "http://127.0.0.1:$port/api/query" \
    --header 'Content-Type: application/json' --data '{"query":"INSERT 42 100"}')"
grep -q '"code":"DUPLICATE_KEY"' <<<"$duplicate_response"
grep -q '409$' <<<"$duplicate_response"
query 'SELECT 999' | grep -q 'not found'

if [[ "$(curl --silent --output /dev/null --write-out '%{http_code}' --request POST \
    "http://127.0.0.1:$port/api/query" --header 'Content-Type: application/json' --data '{}')" != "400" ]]; then
    echo 'Expected POST /api/query with a missing query field to return HTTP 400.' >&2
    exit 1
fi

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=""

echo 'Verified health, static assets, buffer-pool snapshot, INSERT/SELECT, validation, and SIGTERM shutdown.'
