#!/usr/bin/env bash
set -euo pipefail

SCRY="${SCRY:-./scry}"
SCRY_BENCH="${SCRY_BENCH:-./scry-bench}"
CORPUS="${1:-}"
RUNS="${RUNS:-10}"

echo "================================================================"
echo "  scry Reproducible Multi-Engine Benchmark Suite"
echo "================================================================"
echo "CPU: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
echo "scry: $("$SCRY" --version)"
command -v rg >/dev/null 2>&1 && echo "rg: $(rg --version | head -n1)"
command -v ugrep >/dev/null 2>&1 && echo "ugrep: $(ugrep --version | head -n1)"
command -v zoekt >/dev/null 2>&1 && echo "zoekt: installed"
command -v git >/dev/null 2>&1 && echo "git: $(git --version)"
echo ""

# 1. Run compiled C++ micro/macro benchmark (p50/p95/p99, build MB/s, index ratio, delta, RSS)
if [ -n "$CORPUS" ]; then
    "$SCRY_BENCH" --corpus "$CORPUS" --iters 30
else
    "$SCRY_BENCH" --files 5000 --lines 400 --iters 30
fi

# 2. End-to-end CLI wall-clock comparison against rg / git grep / ugrep
TMP_CORPUS=""
if [ -z "$CORPUS" ]; then
    TMP_CORPUS=$(mktemp -d /tmp/scry_cli_bench_XXXXXX)
    trap 'rm -rf "$TMP_CORPUS"' EXIT
    echo ""
    echo "Generating 80 MiB multi-file CLI benchmark corpus in $TMP_CORPUS..."
    python3 - "$TMP_CORPUS" <<'PY'
import os, sys
root = sys.argv[1]
mods = ["kernel/sched", "kernel/mm", "llvm/IR", "llvm/CodeGen", "drivers/net", "src/core"]
for m in mods:
    os.makedirs(os.path.join(root, m), exist_ok=True)
block = (
    "    uint64_t val = compute_trigram_hash(ptr, len);\n"
    "    if (val & 0x1) { return handle_fast_path(ctx, val); }\n"
    "    // benchmark payload line for realistic code search throughput\n"
    "    for (int k = 0; k < 8; ++k) { sum += buckets[k]; }\n"
) * 120
for i in range(3000):
    ext = ".cpp" if i % 3 else ".hpp"
    p = os.path.join(root, mods[i % len(mods)], f"module_{i}{ext}")
    with open(p, "w") as f:
        f.write(f"// file {i}\nstruct Unit_{i} {{ int x; }};\n")
        f.write(block)
        if i == 1234:
            f.write("struct SchedEntityTarget { uint64_t vruntime; };\n")
            f.write("void update_sched_entity_fair_vruntime(SchedEntityTarget* se) {}\n")
        if i % 400 == 7:
            f.write(f"static int special_regex_hook_{i}(int a) {{ return a + 1; }}\n")
PY
    CORPUS="$TMP_CORPUS"
fi

echo ""
echo "=== End-to-End CLI Comparison (median of $RUNS warm runs, corpus=$CORPUS) ==="
"$SCRY" index "$CORPUS"

bench_cmd() {
    local label="$1"
    shift
    python3 - "$label" "$RUNS" "$@" <<'PY'
import subprocess, sys, time
label = sys.argv[1]
runs = int(sys.argv[2])
cmd = sys.argv[3:]
# Warmup
subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
times = []
for _ in range(runs):
    t0 = time.perf_counter()
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    times.append((time.perf_counter() - t0) * 1000.0)
times.sort()
p50 = times[len(times)//2]
p99 = times[int(len(times)*0.99)]
print(f"  {label:<32} p50={p50:7.2f} ms | p99={p99:7.2f} ms")
PY
}

echo ""
echo "-- Query 1: Rare literal ('update_sched_entity_fair_vruntime') --"
bench_cmd "scry (mmap index)" "$SCRY" --no-daemon "update_sched_entity_fair_vruntime" "$CORPUS"
command -v rg >/dev/null 2>&1 && bench_cmd "ripgrep (rg)" rg "update_sched_entity_fair_vruntime" "$CORPUS"
command -v ugrep >/dev/null 2>&1 && bench_cmd "ugrep" ugrep -r "update_sched_entity_fair_vruntime" "$CORPUS"

echo ""
echo "-- Query 2: Case-insensitive literal (-i 'schedentitytarget') --"
bench_cmd "scry -i (hybrid lower table)" "$SCRY" --no-daemon -i "schedentitytarget" "$CORPUS"
command -v rg >/dev/null 2>&1 && bench_cmd "ripgrep -i" rg -i "schedentitytarget" "$CORPUS"

echo ""
echo "-- Query 3: Regex ('special_regex_hook_[0-9]+') --"
bench_cmd "scry (regex planner+DFA)" "$SCRY" --no-daemon "special_regex_hook_[0-9]+" "$CORPUS"
command -v rg >/dev/null 2>&1 && bench_cmd "ripgrep (regex)" rg "special_regex_hook_[0-9]+" "$CORPUS"
