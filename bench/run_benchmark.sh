#!/usr/bin/env bash
# run_benchmark.sh : full cxlmm evaluation harness
#
# Runs 4 workloads × 4 placement modes × N iterations.
# Outputs CSV + summary table. Captures numa_maps for managed mode.
#
# Prerequisites:
#   - Module loaded: sudo insmod cxlmm/kmod/cxlmm.ko cxl_node=1 ddr_node=0
#   - Binaries built: cd bench && make
#   - Run as root: sudo ./run_benchmark.sh
#
# Usage:
#   ./run_benchmark.sh [--iters N] [--size-mb N] [--rounds N] [--cxl-node N] [--ddr-node N]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BENCH="$SCRIPT_DIR/cxlmm_bench"
DAEMON="$PROJECT_DIR/cxlmm/daemon/cxlmm_daemon"
LIB_DIR="$PROJECT_DIR/cxlmm/lib"

ITERS=3
SIZE_MB=2048
ROUNDS=30
CXL_NODE=1
DDR_NODE=0
SCAN_MS=500
WORKLOADS="kv graph tsdb phase"
OUTDIR="$SCRIPT_DIR/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iters)     ITERS="$2";     shift 2 ;;
        --size-mb)   SIZE_MB="$2";   shift 2 ;;
        --rounds)    ROUNDS="$2";    shift 2 ;;
        --cxl-node)  CXL_NODE="$2";  shift 2 ;;
        --ddr-node)  DDR_NODE="$2";  shift 2 ;;
        --workloads) WORKLOADS="$2"; shift 2 ;;
        --help)
            sed -n '2,14p' "$0"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo "[bench] $(date +%H:%M:%S) $*"; }
die() { echo "[bench] ERROR: $*" >&2; exit 1; }

# Pre-flight checks
[[ "$(id -u)" -eq 0 ]] || die "must be run as root"
[[ -x "$BENCH" ]]      || die "cxlmm_bench not built : run 'make' in bench/"
[[ -x "$DAEMON" ]]     || die "cxlmm_daemon not built : run 'make' in cxlmm/"
[[ -f "$LIB_DIR/libcxlmm.so" ]] || die "libcxlmm.so not built"

mkdir -p "$OUTDIR"
CSV="$OUTDIR/results_${TIMESTAMP}.csv"
SUMMARY="$OUTDIR/summary_${TIMESTAMP}.txt"

echo "workload,mode,iter,round,time_ms" > "$CSV"

# -------------------------------------------------------------------------
# Run a single benchmark configuration
# -------------------------------------------------------------------------
run_config() {
    local workload="$1" mode="$2" iter="$3"
    local tmpout=$(mktemp)
    local DAEMON_PID="" BENCH_PID=""

    cleanup_run() {
        [[ -n "$BENCH_PID" ]]  && kill -0 "$BENCH_PID"  2>/dev/null && kill "$BENCH_PID"  2>/dev/null || true
        [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null && kill "$DAEMON_PID" 2>/dev/null || true
        rm -f "$tmpout"
    }
    trap cleanup_run RETURN

    local bench_args=(--workload="$workload" --rounds="$ROUNDS" --size-mb="$SIZE_MB" --csv --hold=20)

    case "$mode" in
        ddr)
            bench_args+=(--no-cxlmm)
            log "  [$workload] $mode iter=$iter: numactl --membind=$DDR_NODE"
            env LD_LIBRARY_PATH="$LIB_DIR" \
                numactl --membind="$DDR_NODE" "$BENCH" "${bench_args[@]}" > "$tmpout" 2>&1 &
            BENCH_PID=$!
            wait "$BENCH_PID" || true
            BENCH_PID=""
            ;;
        cxl)
            bench_args+=(--no-cxlmm)
            log "  [$workload] $mode iter=$iter: numactl --membind=$CXL_NODE"
            env LD_LIBRARY_PATH="$LIB_DIR" \
                numactl --membind="$CXL_NODE" "$BENCH" "${bench_args[@]}" > "$tmpout" 2>&1 &
            BENCH_PID=$!
            wait "$BENCH_PID" || true
            BENCH_PID=""
            ;;
        default)
            bench_args+=(--no-cxlmm)
            log "  [$workload] $mode iter=$iter: OS default placement"
            env LD_LIBRARY_PATH="$LIB_DIR" "$BENCH" "${bench_args[@]}" > "$tmpout" 2>&1 &
            BENCH_PID=$!
            wait "$BENCH_PID" || true
            BENCH_PID=""
            ;;
        managed)
            log "  [$workload] $mode iter=$iter: cxlmm-managed"

            if [[ ! -c /dev/cxlmm ]]; then
                log "  SKIP: /dev/cxlmm not found (module not loaded)"
                return
            fi

            env LD_LIBRARY_PATH="$LIB_DIR" "$BENCH" "${bench_args[@]}" > "$tmpout" 2>&1 &
            BENCH_PID=$!
            sleep 1

            if ! kill -0 "$BENCH_PID" 2>/dev/null; then
                log "  FAIL: benchmark exited immediately"
                BENCH_PID=""
                return
            fi

            # Start daemon
            env LD_LIBRARY_PATH="$LIB_DIR" "$DAEMON" \
                --cxl-node "$CXL_NODE" --ddr-node "$DDR_NODE" \
                --track-pid "$BENCH_PID" --scan-ms "$SCAN_MS" \
                --verbose > "$OUTDIR/daemon_${workload}_${iter}.log" 2>&1 &
            DAEMON_PID=$!

            # Capture numa_maps after benchmark rounds + daemon classification time
            sleep $(( ROUNDS * 200 / 1000 + 10 ))
            if kill -0 "$BENCH_PID" 2>/dev/null; then
                cat "/proc/$BENCH_PID/numa_maps" > \
                    "$OUTDIR/numa_maps_${workload}_${iter}.txt" 2>/dev/null || true
            fi

            wait "$BENCH_PID" || true
            BENCH_PID=""

            # Capture final /proc/cxlmm/stats
            cat /proc/cxlmm/stats > "$OUTDIR/stats_${workload}_${iter}.txt" 2>/dev/null || true

            sleep 2
            if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
                kill "$DAEMON_PID"
                wait "$DAEMON_PID" 2>/dev/null || true
            fi
            DAEMON_PID=""
            ;;
    esac

    # Parse CSV output from bench and append to main CSV
    grep "^[0-9]" "$tmpout" | while IFS=, read -r round time_ms wl md; do
        echo "$workload,$mode,$iter,$round,$time_ms"
    done >> "$CSV" || true
}

# -------------------------------------------------------------------------
# Main loop
# -------------------------------------------------------------------------
log "=== cxlmm benchmark suite ==="
log "workloads: $WORKLOADS"
log "modes: ddr cxl default managed"
log "iterations: $ITERS  rounds: $ROUNDS  size: ${SIZE_MB}MB"
log "output: $CSV"
echo

for workload in $WORKLOADS; do
    log "--- Workload: $workload ---"
    for mode in ddr cxl default managed; do
        for iter in $(seq 1 "$ITERS"); do
            run_config "$workload" "$mode" "$iter"
        done
    done
    echo
done

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------
log "=== Generating summary ==="

{
    echo "cxlmm Benchmark Results : $(date)"
    echo "Hardware: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
    echo "Kernel: $(uname -r)"
    echo "NUMA: node $DDR_NODE (DDR), node $CXL_NODE (CXL)"
    echo "Config: ${SIZE_MB}MB working set, $ROUNDS rounds, $ITERS iterations"
    echo
    echo "=== Average time per round (ms) ==="
    printf "%-12s %10s %10s %10s %10s\n" "workload" "DDR" "CXL" "OS-default" "cxlmm"
    printf "%-12s %10s %10s %10s %10s\n" "--------" "---" "---" "----------" "-----"

    for workload in $WORKLOADS; do
        ddr_avg=$(awk -F, -v w="$workload" -v m="ddr" \
            '$1==w && $2==m {sum+=$5; n++} END {if(n>0) printf "%.1f", sum/n; else print "N/A"}' "$CSV")
        cxl_avg=$(awk -F, -v w="$workload" -v m="cxl" \
            '$1==w && $2==m {sum+=$5; n++} END {if(n>0) printf "%.1f", sum/n; else print "N/A"}' "$CSV")
        def_avg=$(awk -F, -v w="$workload" -v m="default" \
            '$1==w && $2==m {sum+=$5; n++} END {if(n>0) printf "%.1f", sum/n; else print "N/A"}' "$CSV")
        mgd_avg=$(awk -F, -v w="$workload" -v m="managed" \
            '$1==w && $2==m {sum+=$5; n++} END {if(n>0) printf "%.1f", sum/n; else print "N/A"}' "$CSV")

        printf "%-12s %10s %10s %10s %10s\n" "$workload" "$ddr_avg" "$cxl_avg" "$def_avg" "$mgd_avg"
    done

    echo
    echo "=== Page placement verification ==="
    for f in "$OUTDIR"/numa_maps_*.txt; do
        [[ -f "$f" ]] || continue
        echo "--- $(basename "$f") ---"
        # Count pages per node
        awk '{
            for(i=1;i<=NF;i++) {
                if($i ~ /^N[0-9]+=/) {
                    split($i, a, "=");
                    node[a[1]] += a[2]
                }
            }
        } END {
            for(n in node) printf "  %s: %d pages (%.1f MB)\n", n, node[n], node[n]*4/1024
        }' "$f"
    done

    echo
    echo "=== Kernel stats (last managed run) ==="
    for f in "$OUTDIR"/stats_*.txt; do
        [[ -f "$f" ]] || continue
        echo "--- $(basename "$f") ---"
        cat "$f"
    done

} | tee "$SUMMARY"

log "CSV:     $CSV"
log "Summary: $SUMMARY"
log "Done."
