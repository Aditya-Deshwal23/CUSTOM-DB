#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:?Usage: check_documented_sql.sh SERVER_BINARY}"
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
work_dir="$(mktemp -d)"
server_pid=""
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
log_file="$work_dir/server.log"

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -f "$log_file" "$work_dir/database"
    rmdir "$work_dir"
}
trap cleanup EXIT

cd "$repo_root"
PORT="$port" DB_PATH="$work_dir/database" "$server_bin" >"$log_file" 2>&1 &
server_pid=$!

for _ in {1..50}; do
    if curl --silent --fail "http://127.0.0.1:$port/health" >/dev/null; then
        break
    fi
    sleep 0.1
done

query() {
    curl --silent --fail --request POST "http://127.0.0.1:$port/api/query" \
        --header 'Content-Type: application/json' \
        --data "{\"query\":\"$1\"}" |
        python3 -c 'import json, sys; print(json.load(sys.stdin)["result"])'
}

insert_result="$(query 'INSERT 42 100')"
[[ "$insert_result" == "Inserted (42, 100)" ]]
select_result="$(query 'SELECT 42')"
[[ "$select_result" == "Result: 100" ]]

legacy_sql_result="$(query 'SELECT * FROM Users')"
if [[ "$legacy_sql_result" == *"Syntax error"* || "$legacy_sql_result" == *"Unknown command"* ]]; then
    echo "Documented SQL example is not supported by the live server: $legacy_sql_result" >&2
    exit 1
fi

echo "Verified documented integer INSERT/SELECT examples against the live Crow server."
