#!/usr/bin/env bash
# memcached_baseline.sh : memcached DDR vs CXL placement comparison
#
# Runs memcached under different NUMA placements and benchmarks with
# memtier_benchmark to show real-world impact of memory placement.
#
# Usage: sudo ./memcached_baseline.sh [--cxl-node 1] [--ddr-node 0]

set -euo pipefail

CXL_NODE=1
DDR_NODE=0
DATA_SIZE=1024       # 1 GB of data
THREADS=4
CLIENTS=50
REQUESTS=1000000
PORT_BASE=11300

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cxl-node) CXL_NODE="$2"; shift 2 ;;
        --ddr-node) DDR_NODE="$2"; shift 2 ;;
        --help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "Unknown: $1"; exit 1 ;;
    esac
done

OUTDIR="$(dirname "$0")/results"
mkdir -p "$OUTDIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY="$OUTDIR/memcached_${TIMESTAMP}.txt"

log() { echo "[memcached] $(date +%H:%M:%S) $*"; }

[[ "$(id -u)" -eq 0 ]] || { echo "Run as root"; exit 1; }
which memcached >/dev/null || { echo "memcached not found"; exit 1; }
which memtier_benchmark >/dev/null || { echo "memtier_benchmark not found"; exit 1; }

run_memcached_test() {
    local label="$1" numa_args="$2" port="$3"
    local MC_PID=""

    log "=== $label ==="

    # Start memcached
    $numa_args memcached -p "$port" -m "$DATA_SIZE" -t "$THREADS" -u root -d -P "/tmp/mc_${port}.pid" 2>/dev/null
    sleep 1
    MC_PID=$(cat "/tmp/mc_${port}.pid" 2>/dev/null || true)

    if [[ -z "$MC_PID" ]] || ! kill -0 "$MC_PID" 2>/dev/null; then
        log "  FAIL: memcached did not start"
        return
    fi

    log "  memcached PID=$MC_PID, port=$port"

    # Phase 1: populate (SET-heavy)
    log "  populating..."
    memtier_benchmark --server=127.0.0.1 --port="$port" \
        --protocol=memcache_text \
        --threads=4 --clients=10 \
        --ratio=1:0 --key-maximum=500000 --data-size=1024 \
        --requests=500000 --hide-histogram \
        2>&1 | tail -3

    # Phase 2: read-heavy workload (1:10 set:get ratio)
    log "  running read-heavy benchmark..."
    memtier_benchmark --server=127.0.0.1 --port="$port" \
        --protocol=memcache_text \
        --threads="$THREADS" --clients="$CLIENTS" \
        --ratio=1:10 --key-maximum=500000 --data-size=1024 \
        --requests="$REQUESTS" --hide-histogram \
        2>&1 | tee "$OUTDIR/memtier_${label}_${TIMESTAMP}.txt"

    # Capture numa_maps
    cat "/proc/$MC_PID/numa_maps" > "$OUTDIR/memcached_numa_maps_${label}_${TIMESTAMP}.txt" 2>/dev/null || true

    # Count pages per node
    log "  page placement:"
    awk '{
        for(i=1;i<=NF;i++) {
            if($i ~ /^N[0-9]+=/) {
                split($i, a, "=");
                node[a[1]] += a[2]
            }
        }
    } END {
        for(n in node) printf "    %s: %d pages (%.1f MB)\n", n, node[n], node[n]*4/1024
    }' "$OUTDIR/memcached_numa_maps_${label}_${TIMESTAMP}.txt"

    # Stop memcached
    kill "$MC_PID" 2>/dev/null
    wait "$MC_PID" 2>/dev/null || true
    rm -f "/tmp/mc_${port}.pid"
    sleep 1
}

{
    echo "Memcached Placement Benchmark : $(date)"
    echo "Hardware: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
    echo "Config: ${DATA_SIZE}MB data, ${THREADS} threads, ${CLIENTS} clients"
    echo

    run_memcached_test "DDR_only"   "numactl --membind=$DDR_NODE"   $((PORT_BASE))
    echo
    run_memcached_test "CXL_only"   "numactl --membind=$CXL_NODE"   $((PORT_BASE+1))
    echo
    run_memcached_test "interleave" "numactl --interleave=all"       $((PORT_BASE+2))
    echo
    run_memcached_test "OS_default" ""                               $((PORT_BASE+3))

} 2>&1 | tee "$SUMMARY"

log "Results: $SUMMARY"
