#!/usr/bin/env bash
set -euo pipefail

port="${PORT:-18080}"
server_bin="${SERVER_BIN:-./build/relational_db}"
work_dir="$(mktemp -d)"
log_file="$work_dir/server.log"
server_pid=""
current_claim="starting verification"

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$work_dir"
}
trap cleanup EXIT
trap 'status=$?; printf "FAIL | %s\n" "$current_claim" >&2; exit "$status"' ERR

pass_claim() {
    printf 'PASS | %s\n' "$current_claim"
}

current_claim="Release server build"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
pass_claim

PORT="$port" DB_PATH="$work_dir/verify.db" "$server_bin" >"$log_file" 2>&1 &
server_pid=$!

current_claim="service readiness"
ready=false
for _ in {1..50}; do
    if curl --silent --fail "http://127.0.0.1:$port/health" |
        grep -Eq '"status"[[:space:]]*:[[:space:]]*"ok"'; then
        ready=true
        break
    fi
    sleep 0.1
done
[[ "$ready" == true ]]
pass_claim

current_claim="dashboard static assets"
curl --silent --fail "http://127.0.0.1:$port/" | grep -qi '<!doctype html>'
curl --silent --fail "http://127.0.0.1:$port/index.css" >/dev/null
curl --silent --fail "http://127.0.0.1:$port/index.js" >/dev/null
pass_claim

current_claim="legacy buffer-pool endpoint"
buffer_pool="$(curl --silent --fail "http://127.0.0.1:$port/api/buffer_pool")"
grep -q '"pinned_frames"' <<<"$buffer_pool"
grep -q '"frames"' <<<"$buffer_pool"
pass_claim

current_claim="live metrics endpoint"
metrics="$(curl --silent --fail "http://127.0.0.1:$port/api/metrics")"
grep -q '"query_throughput"' <<<"$metrics"
grep -q '"source":"live"' <<<"$metrics"
grep -q '"cache_hit_rate"' <<<"$metrics"
grep -q '"avg_latency"' <<<"$metrics"
grep -q '"error_count"' <<<"$metrics"
pass_claim

query() {
    curl --silent --fail --request POST "http://127.0.0.1:$port/api/query" \
        --header 'Content-Type: application/json' \
        --data "{\"query\":\"$1\"}"
}

current_claim="legacy INSERT and SELECT aliases"
query 'INSERT 42 100' | grep -q 'Inserted (42, 100)'
query 'SELECT 42' | grep -q 'Result: 100'
pass_claim

current_claim="duplicate-key error contract"
duplicate_response="$(curl --silent --write-out $'\n%{http_code}' \
    --request POST "http://127.0.0.1:$port/api/query" \
    --header 'Content-Type: application/json' --data '{"query":"INSERT 42 100"}')"
grep -q '"code":"DUPLICATE_KEY"' <<<"$duplicate_response"
grep -q '409$' <<<"$duplicate_response"
pass_claim

current_claim="missing-row response"
query 'SELECT 999' | grep -q 'not found'
pass_claim

current_claim="missing-query validation"
missing_query_status="$(curl --silent --output /dev/null --write-out '%{http_code}' --request POST \
    "http://127.0.0.1:$port/api/query" --header 'Content-Type: application/json' --data '{}')"
[[ "$missing_query_status" == "400" ]]
pass_claim

current_claim="graceful SIGTERM shutdown"
kill -TERM "$server_pid"
wait "$server_pid"
server_pid=""
pass_claim
