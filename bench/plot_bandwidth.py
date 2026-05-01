#!/usr/bin/env python3
"""
Plot aggregate bandwidth from cxlmm benchmark results.

Bandwidth = data_touched_per_round / time_per_round

Per-round data touched (based on cxlmm_bench.c with 2048 MB, 2M ops):
  kv:    2M ops * 64 B (cache line) = 128 MB
  graph: ~1843 MB adj (read) + offsets + 2*rank (read+write) ≈ 1930 MB
  tsdb:  1.2M*64 B (write) + 0.8M*4096 B (read) ≈ 3354 MB
  phase: 524288 page-touches * 8 B = 4 MB (split into write/read halves)
"""

import csv
import sys
from collections import defaultdict

# ---------- constants matching the benchmark ----------
SIZE_MB = 2048
OPS_PER_ROUND = 2_000_000
PAGE_SIZE = 4096
CACHE_LINE = 64

# Data touched per round (MB)
BYTES_PER_ROUND = {
    "kv":    OPS_PER_ROUND * CACHE_LINE,                              # 128 MB
    "graph": int(SIZE_MB * 0.9) * 1024 * 1024                         # adj read
             + (int(SIZE_MB * 0.9) * 1024 * 1024 // 4 // 16 + 1) * 4  # offsets
             + 2 * (int(SIZE_MB * 0.9) * 1024 * 1024 // 4 // 16) * 4  # rank r+w
             + (int(SIZE_MB * 0.9) * 1024 * 1024 // 4 // 16) * 4,     # rank_new memset
    "tsdb":  int(OPS_PER_ROUND * 0.6) * CACHE_LINE                    # ring writes
             + int(OPS_PER_ROUND * 0.4) * PAGE_SIZE,                   # index reads
    "phase": (SIZE_MB * 1024 * 1024 // PAGE_SIZE) * 8,                # one 8B per page
}

DATA_MB = {k: v / (1024 * 1024) for k, v in BYTES_PER_ROUND.items()}

CSV_PATH = "results/results_20260415_033442.csv"

# ---------- read data ----------
records = defaultdict(list)  # (workload, mode) -> [time_ms, ...]

with open(CSV_PATH) as f:
    reader = csv.DictReader(f)
    for row in reader:
        wl = row["workload"]
        mode = row["mode"]
        t = float(row["time_ms"])
        records[(wl, mode)].append(t)

# ---------- compute bandwidth ----------
workloads = ["kv", "tsdb", "graph", "phase"]
modes = ["ddr", "cxl", "default", "managed"]
mode_labels = {
    "ddr": "DDR Only",
    "cxl": "CXL Only",
    "default": "OS Default",
    "managed": "cxlmm",
}

# Colors
mode_colors = {
    "ddr":     "#2563eb",  # blue
    "cxl":     "#dc2626",  # red
    "default": "#6b7280",  # gray
    "managed": "#16a34a",  # green
}

print("=" * 70)
print(f"{'Workload':<10} {'Mode':<12} {'Avg ms':>8} {'Data/rnd MB':>12} {'BW (GB/s)':>10}")
print("-" * 70)

bw = {}  # (workload, mode) -> GB/s
for wl in workloads:
    for mode in modes:
        key = (wl, mode)
        times = records.get(key, [])
        if not times:
            continue
        avg_ms = sum(times) / len(times)
        data_mb = DATA_MB[wl]
        bw_gbs = (data_mb / 1024) / (avg_ms / 1000)  # GB/s
        bw[key] = bw_gbs
        print(f"{wl:<10} {mode:<12} {avg_ms:>8.1f} {data_mb:>12.1f} {bw_gbs:>10.2f}")

print("=" * 70)

# ---------- generate SVG bar chart ----------
n_workloads = len(workloads)
n_modes = len(modes)
bar_w = 40
bar_gap = 8
group_gap = 60
group_w = n_modes * (bar_w + bar_gap) - bar_gap

chart_w = n_workloads * (group_w + group_gap) - group_gap
margin_l = 80
margin_r = 30
margin_t = 50
margin_b = 100
legend_h = 30

svg_w = margin_l + chart_w + margin_r
chart_h = 350
svg_h = margin_t + chart_h + margin_b + legend_h

max_bw = max(bw.values()) * 1.15

def y(val):
    return margin_t + chart_h - (val / max_bw) * chart_h

lines = []
lines.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {svg_w} {svg_h}" width="{svg_w}" height="{svg_h}">')
lines.append('<style>')
lines.append('  text { font-family: "Helvetica Neue", Arial, sans-serif; }')
lines.append('  .title { font-size: 16px; font-weight: bold; fill: #1f2937; }')
lines.append('  .axis-label { font-size: 12px; fill: #374151; }')
lines.append('  .tick-label { font-size: 11px; fill: #6b7280; }')
lines.append('  .bar-label { font-size: 10px; fill: #374151; font-weight: 500; }')
lines.append('  .legend-text { font-size: 11px; fill: #374151; }')
lines.append('  .gridline { stroke: #e5e7eb; stroke-width: 1; }')
lines.append('</style>')

# Title
lines.append(f'<text x="{svg_w/2}" y="28" text-anchor="middle" class="title">Aggregate Bandwidth by Workload and Memory Placement</text>')

# Y-axis gridlines and labels
n_ticks = 6
for i in range(n_ticks + 1):
    val = max_bw * i / n_ticks
    yy = y(val)
    lines.append(f'<line x1="{margin_l}" y1="{yy}" x2="{margin_l + chart_w}" y2="{yy}" class="gridline"/>')
    lines.append(f'<text x="{margin_l - 8}" y="{yy + 4}" text-anchor="end" class="tick-label">{val:.1f}</text>')

# Y-axis label
lines.append(f'<text x="18" y="{margin_t + chart_h/2}" text-anchor="middle" transform="rotate(-90, 18, {margin_t + chart_h/2})" class="axis-label">Bandwidth (GB/s)</text>')

# Bars
for wi, wl in enumerate(workloads):
    gx = margin_l + wi * (group_w + group_gap)
    # Workload label
    cx = gx + group_w / 2
    lines.append(f'<text x="{cx}" y="{margin_t + chart_h + 20}" text-anchor="middle" class="axis-label">{wl.upper()}</text>')

    for mi, mode in enumerate(modes):
        key = (wl, mode)
        if key not in bw:
            continue
        val = bw[key]
        bx = gx + mi * (bar_w + bar_gap)
        by = y(val)
        bh = margin_t + chart_h - by
        color = mode_colors[mode]

        # Bar with rounded top
        lines.append(f'<rect x="{bx}" y="{by}" width="{bar_w}" height="{bh}" fill="{color}" rx="3" ry="3"/>')
        # Value label above bar
        lines.append(f'<text x="{bx + bar_w/2}" y="{by - 5}" text-anchor="middle" class="bar-label">{val:.1f}</text>')

# Legend
ly = margin_t + chart_h + 55
lx_start = margin_l
for mi, mode in enumerate(modes):
    lx = lx_start + mi * 150
    color = mode_colors[mode]
    lines.append(f'<rect x="{lx}" y="{ly}" width="14" height="14" fill="{color}" rx="2"/>')
    lines.append(f'<text x="{lx + 20}" y="{ly + 11}" class="legend-text">{mode_labels[mode]}</text>')

# Axes
lines.append(f'<line x1="{margin_l}" y1="{margin_t}" x2="{margin_l}" y2="{margin_t + chart_h}" stroke="#9ca3af" stroke-width="1.5"/>')
lines.append(f'<line x1="{margin_l}" y1="{margin_t + chart_h}" x2="{margin_l + chart_w}" y2="{margin_t + chart_h}" stroke="#9ca3af" stroke-width="1.5"/>')

lines.append('</svg>')

svg_path = "../report/assets/aggregate_bandwidth.svg"
with open(svg_path, "w") as f:
    f.write("\n".join(lines))

print(f"\nSVG written to {svg_path}")
