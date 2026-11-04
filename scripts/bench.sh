#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/bench.sh [--smoke]

Build and run the fair in-process comparison benchmark. The default matrix
covers 10k/100k/1M rows, 1/2/4/8 threads, point/range/insert/update and YCSB
A/B/C/E workloads. Results are JSON Lines on stdout; set OUTPUT to save them.
Inserts are run with WAL synchronous=OFF and with FULL/NORMAL durability.
JOIN is omitted because this checkout does not implement SQL JOIN.

Environment overrides:
  BUILD_DIR, BENCHMARK_ROWS, BENCHMARK_THREADS, BENCHMARK_OPERATIONS,
  BENCHMARK_WORKLOADS, BENCHMARK_REPETITIONS, SEED, OUTPUT
EOF
}

if (($# > 1)); then
    usage >&2
    exit 2
fi

smoke=false
if (($# == 1)); then
    if [[ "$1" == "--help" ]]; then
        usage
        exit 0
    elif [[ "$1" == "--smoke" ]]; then
        smoke=true
    else
        usage >&2
        exit 2
    fi
fi

build_dir="${BUILD_DIR:-build-bench}"
rows="${BENCHMARK_ROWS:-10000,100000,1000000}"
threads="${BENCHMARK_THREADS:-1,2,4,8}"
operations="${BENCHMARK_OPERATIONS:-10000}"
repetitions="${BENCHMARK_REPETITIONS:-5}"
workloads="${BENCHMARK_WORKLOADS:-point,range100,insert,durable_insert,update,ycsb_a,ycsb_b,ycsb_c,ycsb_e}"
seed="${SEED:-1337}"
output="${OUTPUT:-}"

if [[ "$smoke" == true ]]; then
    rows="1000"
    threads="1"
    operations="50"
    repetitions="1"
    workloads="point,range100,insert,durable_insert,update,ycsb_a,ycsb_b,ycsb_c,ycsb_e"
fi

for positive_value in "$operations" "$repetitions"; do
    if [[ ! "$positive_value" =~ ^[1-9][0-9]*$ ]]; then
        echo "Expected a positive integer, got: $positive_value" >&2
        exit 2
    fi
done
if [[ ! "$seed" =~ ^[0-9]+$ ]]; then
    echo "Expected a non-negative integer seed, got: $seed" >&2
    exit 2
fi

if [[ ",$workloads," == *",join,"* ]]; then
    echo "JOIN benchmark is unavailable: this checkout's SQL parser does not implement JOIN." >&2
    exit 2
fi

cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
    -DRELDB_BUILD_TESTS=OFF -DRELDB_BUILD_BENCHMARKS=OFF \
    -DRELDB_BUILD_SQLITE_BENCHMARK=ON
cmake --build "$build_dir" --config Release --parallel --target reldb_fair_bench

if [[ -n "$output" ]]; then
    mkdir -p "$(dirname "$output")"
    : > "$output"
fi

IFS=, read -r -a row_values <<<"$rows"
IFS=, read -r -a thread_values <<<"$threads"
IFS=, read -r -a workload_values <<<"$workloads"
for row_count in "${row_values[@]}"; do
    for thread_count in "${thread_values[@]}"; do
        for workload in "${workload_values[@]}"; do
            case "$workload" in
                insert)
                    durability_values=(off)
                    ;;
                durable_insert)
                    durability_values=(full normal)
                    ;;
                *)
                    durability_values=(full)
                    ;;
            esac
            for durability in "${durability_values[@]}"; do
                for ((repetition = 0; repetition < repetitions; repetition++)); do
                    run_seed=$((seed + repetition))
                    printf 'Running rows=%s threads=%s workload=%s durability=%s repetition=%s\n' \
                        "$row_count" "$thread_count" "$workload" "$durability" "$repetition" >&2
                    if [[ -n "$output" ]]; then
                        "$build_dir/reldb_fair_bench" --rows "$row_count" \
                            --operations "$operations" --threads "$thread_count" --seed "$run_seed" \
                            --workload "$workload" --durability "$durability" >> "$output"
                    else
                        "$build_dir/reldb_fair_bench" --rows "$row_count" \
                            --operations "$operations" --threads "$thread_count" --seed "$run_seed" \
                            --workload "$workload" --durability "$durability"
                    fi
                done
            done
        done
    done
done
