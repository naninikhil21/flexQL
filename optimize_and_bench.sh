#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

MAX_ITERATIONS=15
BENCHMARK_TIMEOUT_SECONDS=90
TARGET_MS=400
ROWS=1000000

BEST_MS=999999999
BEST_ITER=0
BEST_BATCH=0

ITER_RESULTS=()

set_batch_size() {
    local batch="$1"
    sed -i -E "s/(static const int INSERT_BATCH_SIZE = )[0-9]+;/\\1${batch};/" benchmark_flexql.cpp
}

build_all() {
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/tmp/fql_cmake_cfg.log 2>&1
    cmake --build build --target flexql_server -j"$(nproc)" >/tmp/fql_cmake_build.log 2>&1
    g++ flexql.cpp benchmark_flexql.cpp -O2 -o benchmark >/tmp/fql_bench_build.log 2>&1
}

kill_server() {
    pkill -f "build/flexql_server" 2>/dev/null || true
    pkill -f "flexql_server" 2>/dev/null || true
    sleep 0.5
}

wait_server_ready() {
    local timeout_ms=5000
    local elapsed=0
    while (( elapsed < timeout_ms )); do
        if ss -ltn 2>/dev/null | grep -q ':9000 '; then
            return 0
        fi
        sleep 0.1
        elapsed=$((elapsed + 100))
    done
    return 1
}

run_unit_tests() {
    kill_server
    : > flexql.wal
    ./build/flexql_server 9000 >/tmp/fql_server_unit.log 2>&1 &
    if ! wait_server_ready; then
        return 1
    fi
    if ! timeout 45s ./benchmark --unit-test >/tmp/fql_unit.log 2>&1; then
        kill_server
        return 1
    fi
    kill_server
    return 0
}

parse_elapsed() {
    grep -E '^Elapsed:' "$1" | grep -oE '[0-9]+' | head -1
}

parse_throughput() {
    grep -E '^Throughput:' "$1" | grep -oE '[0-9]+' | head -1
}

BATCH_CANDIDATES=(5000 10000 25000)

for ((iteration=1; iteration<=MAX_ITERATIONS; iteration++)); do
    idx=$(( (iteration - 1) % ${#BATCH_CANDIDATES[@]} ))
    BATCH_SIZE="${BATCH_CANDIDATES[$idx]}"

    echo "=== Iteration ${iteration}: batch=${BATCH_SIZE}, async-persist SQL server ==="

    set_batch_size "$BATCH_SIZE"

    if ! build_all; then
        echo "Build: FAILED"
        ITER_RESULTS+=("${iteration}|BUILD_FAILED|0|0|SKIPPED|ERROR")
        continue
    fi
    echo "Build: OK"

    kill_server
    ./build/flexql_server 9000 >/tmp/fql_server.log 2>&1 &

    if ! wait_server_ready; then
        echo "Server start: FAILED"
        kill_server
        ITER_RESULTS+=("${iteration}|SERVER_FAILED|0|0|SKIPPED|ERROR")
        continue
    fi
    echo "Server start: OK"

    BENCH_LOG="/tmp/fql_bench_${iteration}.log"
    if ! timeout "${BENCHMARK_TIMEOUT_SECONDS}s" ./benchmark "$ROWS" >"$BENCH_LOG" 2>&1; then
        if grep -q "Terminated\|timed out" "$BENCH_LOG" 2>/dev/null; then
            echo "Benchmark result: TIMEOUT"
            kill_server
            ITER_RESULTS+=("${iteration}|TIMEOUT|0|0|SKIPPED|TIMEOUT")
            continue
        fi
        echo "Benchmark result: ERROR"
        cat "$BENCH_LOG"
        kill_server
        ITER_RESULTS+=("${iteration}|ERROR|0|0|SKIPPED|ERROR")
        continue
    fi

    ELAPSED_MS="$(parse_elapsed "$BENCH_LOG")"
    THROUGHPUT="$(parse_throughput "$BENCH_LOG")"

    if [[ -z "$ELAPSED_MS" ]]; then
        echo "Benchmark result: ERROR (unable to parse elapsed)"
        cat "$BENCH_LOG"
        kill_server
        ITER_RESULTS+=("${iteration}|PARSE_ERROR|0|0|SKIPPED|ERROR")
        continue
    fi

    UNIT_STATUS="SKIPPED"
    if run_unit_tests; then
        UNIT_STATUS="PASS"
    else
        UNIT_STATUS="FAIL"
    fi

    STATUS="SAME"
    if (( ELAPSED_MS < BEST_MS )); then
        BEST_MS=$ELAPSED_MS
        BEST_ITER=$iteration
        BEST_BATCH=$BATCH_SIZE
        STATUS="IMPROVED"
    elif (( ELAPSED_MS > BEST_MS )); then
        STATUS="REGRESSED"
    fi

    echo "Benchmark result: ${ELAPSED_MS} ms"
    echo "Throughput: ${THROUGHPUT} rows/sec"
    echo "Unit tests: ${UNIT_STATUS}"
    echo "Status: ${STATUS}"
    echo "Best so far: ${BEST_MS} ms (iter=${BEST_ITER}, batch=${BEST_BATCH})"

    ITER_RESULTS+=("${iteration}|${ELAPSED_MS}|${THROUGHPUT}|${BATCH_SIZE}|${UNIT_STATUS}|${STATUS}")

    kill_server

    if (( ELAPSED_MS <= TARGET_MS )) && [[ "$UNIT_STATUS" == "PASS" ]]; then
        echo "TARGET REACHED at iteration ${iteration}: ${ELAPSED_MS}ms"
        break
    fi

done

if (( BEST_ITER == 0 )); then
    echo "Best result: not available"
else
    set_batch_size "$BEST_BATCH"
    build_all
fi

echo ""
echo "=== Final Summary ==="
echo "Best result: ${BEST_MS} ms (iteration ${BEST_ITER}, batch ${BEST_BATCH})"
for item in "${ITER_RESULTS[@]}"; do
    IFS='|' read -r it ms tput batch unit status <<<"$item"
    echo "iter=${it} elapsed=${ms} throughput=${tput} batch=${batch} unit=${unit} status=${status}"
done
