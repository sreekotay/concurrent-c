#!/usr/bin/env bash
# Scheduler regression pass: the spawn/fiber micros, then redis, pigz, staticd.
#
#   ./scripts/sched_perf.sh
#   SCHED_PERF_PRODUCTS=0 ./scripts/sched_perf.sh
#
# Logs land in ${SCHED_PERF_DIR:-/tmp/sched_perf}/<name>.txt. A failing row
# is recorded and the rest still run. Exit status is 0 only if every row
# passed.
#
# Product binaries are deleted and rebuilt with CC_NO_CACHE so they link
# this tree's runtime, not a cached object from an older scheduler.
# perf_spawn_ladder is omitted: tools/run_all.ccs marks it a machine-crasher
# under aggregate spawn pressure.
#
# Pigz builds the scheduler-linked variants and times the ones that link.
# Redis includes redis_std. If the default ports are taken, the next free
# triple is used (listen failure there is "listen error: 7").
set -u

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

CCC="${CCC:-$ROOT/cc/bin/ccc}"
DIR="${SCHED_PERF_DIR:-/tmp/sched_perf}"
PRODUCTS="${SCHED_PERF_PRODUCTS:-1}"
fails=0

mkdir -p "$DIR"

if [[ ! -x "$CCC" && ! -L "$CCC" ]]; then
    echo "sched_perf: no $CCC (make -C cc)" >&2
    exit 2
fi

run() {
    local name="$1"
    shift
    local log="$DIR/${name}.txt"
    local st
    echo "===== $name ====="
    if "$@" >"$log" 2>&1; then
        st=0
        echo "OK $name"
    else
        st=$?
        fails=$((fails + 1))
        echo "FAIL $name exit:$st"
    fi
    tail -n 8 "$log" || true
    echo
    return 0
}

echo "sched_perf: logs in $DIR"

run spawn_simple "$CCC" run --release perf/spawn_simple.ccs
run spawn_sequential "$CCC" run --release perf/spawn_sequential.ccs
run spawn_nursery "$CCC" run --release perf/spawn_nursery.ccs
run spawn_nursery_simple "$CCC" run --release perf/spawn_nursery_simple.ccs
run spawn_nursery_direct "$CCC" run --release perf/spawn_nursery_direct.ccs
run spawn_fiber_direct "$CCC" run --release perf/spawn_fiber_direct.ccs
run perf_async_overhead "$CCC" run --release perf/perf_async_overhead.ccs
run work_stealing_efficiency "$CCC" run --release perf/work_stealing_efficiency.ccs
run perf_gobench_async_pressure "$CCC" run --release perf/perf_gobench_async_pressure.ccs
run perf_gobench_blocking_pressure "$CCC" run --release perf/perf_gobench_blocking_pressure.ccs
run fiber_overhead_profile "$CCC" run --release perf/fiber_overhead_profile.ccs
run parallel_hello "$CCC" run --release perf/parallel_hello.ccs
run parallel_trace "$CCC" run --release perf/parallel_trace.ccs
run parallel_hello_lowered "$CCC" run --release perf/parallel_hello_lowered.ccs
run mpmc_worker_pool "$CCC" run --release perf/mpmc_worker_pool.ccs
run perf_spawn_v2_vs_thread "$CCC" run --release perf/perf_spawn_v2_vs_thread.ccs
run perf_parallel_handoff "$CCC" run --release perf/perf_parallel_handoff.ccs
run parallel_pow2 env CC_PAR_BARE=0 ./perf/parallel_pow2.shcc

if [[ "$PRODUCTS" == "0" ]]; then
    echo "sched_perf: products skipped (SCHED_PERF_PRODUCTS=0); fails=$fails"
    exit "$([[ $fails -eq 0 ]] && echo 0 || echo 1)"
fi

port_busy() {
    lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1
}

# upstream, idiomatic, std. Shift by 100 until a free triple shows up.
redis_ports() {
    local base=6391
    local try
    for try in 1 2 3 4 5; do
        if ! port_busy "$base" && ! port_busy $((base + 2)) && ! port_busy $((base + 7)); then
            echo "$base $((base + 2)) $((base + 7))"
            return 0
        fi
        base=$((base + 100))
    done
    return 1
}

echo "===== redis ====="
rm -f real_projects/redis/out/redis_idiomatic real_projects/redis/out/redis_std
if CC_NO_CACHE=1 make -C real_projects/redis redis_idiomatic redis_std >"$DIR/redis_build.txt" 2>&1; then
    echo "OK redis_build"
else
    fails=$((fails + 1))
    echo "FAIL redis_build exit:$?"
    tail -n 8 "$DIR/redis_build.txt" || true
fi
if ports=$(redis_ports); then
    set -- $ports
    echo "redis ports upstream=$1 idiomatic=$2 std=$3"
    run redis env INCLUDE_STD=1 INCLUDE_UPSTREAM=1 INCLUDE_IDIOMATIC=1 \
        UPSTREAM_PORT="$1" IDIOMATIC_PORT="$2" STD_PORT="$3" \
        ./real_projects/redis/bench_robust.sh
else
    fails=$((fails + 1))
    echo "FAIL redis: no free port triple from 6391" 
    echo
fi

echo "===== pigz ====="
pigz_ok=""
for t in pigz_channel pigz_pthread pigz_hybrid pigz_idiomatic pigz_parallel pigz_cc; do
    rm -f "real_projects/pigz/out/$t"
    if CC_NO_CACHE=1 make -C real_projects/pigz "$t" >"$DIR/pigz_build_${t}.txt" 2>&1; then
        echo "OK build $t"
        pigz_ok="${pigz_ok:+$pigz_ok,}$t"
    else
        fails=$((fails + 1))
        echo "FAIL build $t exit:$?"
        tail -n 8 "$DIR/pigz_build_${t}.txt" || true
    fi
done
if [[ -x real_projects/pigz/out/pigz ]]; then
    names="pigz${pigz_ok:+,$pigz_ok}"
else
    names="$pigz_ok"
fi
if [[ -n "$names" ]]; then
    run pigz ./real_projects/pigz/bench_defaults.sh 50 5 "$names"
else
    fails=$((fails + 1))
    echo "FAIL pigz: nothing built"
    echo
fi

echo "===== staticd ====="
rm -f real_projects/staticd/out/staticd
if CC_NO_CACHE=1 make -C real_projects/staticd staticd >"$DIR/staticd_build.txt" 2>&1; then
    echo "OK staticd_build"
    run staticd ./real_projects/staticd/compare.sh
else
    fails=$((fails + 1))
    echo "FAIL staticd_build exit:$?"
    tail -n 8 "$DIR/staticd_build.txt" || true
    echo
fi

echo "sched_perf: fails=$fails"
if [[ $fails -eq 0 ]]; then
    exit 0
fi
exit 1
