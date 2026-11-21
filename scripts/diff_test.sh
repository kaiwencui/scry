#!/usr/bin/env bash
set -euo pipefail

SCRY="${SCRY:-./scry}"
TARGET="${1:-.}"

if ! command -v rg >/dev/null 2>&1; then
    echo "ERROR: ripgrep (rg) is required for differential testing"
    exit 1
fi

echo "=== scry vs ripgrep (rg) differential test suite (target=$TARGET) ==="

# Build fresh index for target
"$SCRY" index "$TARGET" >/dev/null

PASS=0
FAIL=0

run_diff() {
    local desc="$1"
    shift
    local rg_args=("$@")

    local scry_idx_out scry_lin_out rg_out
    scry_idx_out=$(mktemp)
    scry_lin_out=$(mktemp)
    rg_out=$(mktemp)

    # Normalize output order (rg parallelizes across files so order is non-deterministic)
    (cd "$TARGET" && rg --no-heading --color=never -n -H "${rg_args[@]}" </dev/null 2>/dev/null || true) | LC_ALL=C sort > "$rg_out"
    (cd "$TARGET" && "$OLDPWD/$SCRY" --no-daemon --color=never -n -H "${rg_args[@]}" </dev/null 2>/dev/null || true) | LC_ALL=C sort > "$scry_idx_out"
    (cd "$TARGET" && "$OLDPWD/$SCRY" --no-index --no-daemon --color=never -n -H "${rg_args[@]}" </dev/null 2>/dev/null || true) | LC_ALL=C sort > "$scry_lin_out"

    local ok=1
    if ! diff -u "$rg_out" "$scry_idx_out" >/dev/null; then
        echo "FAIL [indexed] $desc"
        diff -u "$rg_out" "$scry_idx_out" | head -n 20
        ok=0
    fi
    if ! diff -u "$rg_out" "$scry_lin_out" >/dev/null; then
        echo "FAIL [linear]  $desc"
        diff -u "$rg_out" "$scry_lin_out" | head -n 20
        ok=0
    fi

    rm -f "$scry_idx_out" "$scry_lin_out" "$rg_out"

    if [ "$ok" -eq 1 ]; then
        echo "PASS $desc"
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

run_diff "Rare literal" "bp128_unpack_deltas"
run_diff "Common literal" "std::vector"
run_diff "Case-insensitive (-i)" -i "mmapindex"
run_diff "Smart-case lowercase (-S)" -S "trigramquery"
run_diff "Smart-case mixed (-S)" -S "TrigramQuery"
run_diff "Fixed strings (-F)" -F "std::vector<DocId>"
run_diff "Word boundary (-w)" -w "arena"
run_diff "Short 2-char literal (linear fallback)" "u8"
run_diff "Regex alternation" "(AVX-512|AVX2|NEON)"
run_diff "Regex quantifier + class" "u32 [a-z_]+ = 0;"
run_diff "Glob filter (-g '*.hpp')" -g '*.hpp' "TrigramEntry"
run_diff "Negated glob (-g '!tests/**')" -g '!tests/**' "MmapIndex"
run_diff "Type filter (-t cpp)" -t cpp "encode_posting_list"
run_diff "Count mode (-c)" -c "Trigram"
run_diff "Files with matches (-l)" -l "inotify"
run_diff "Multi-pattern (-e)" -e "bp128_unpack_deltas" -e "load_persisted_repos"
run_diff "Multi-pattern fixed (-F -e)" -F -e "std::vector<DocId>" -e "MmapIndex"
run_diff "Case-sensitive override (-i -s)" -i -s "MmapIndex"
run_diff "Vimgrep format (--vimgrep)" --vimgrep "TrigramEntry"
run_diff "Files without match" --files-without-match "TrigramEntry"
run_diff "Multi-path (src tests)" "bp128_unpack_deltas" src tests

# Stdin piping differential check
stdin_rg=$(mktemp)
stdin_scry=$(mktemp)
rg --color=never -n "CliCommand" < "$TARGET/src/main.cpp" > "$stdin_rg" || true
"$SCRY" --color=never -n "CliCommand" < "$TARGET/src/main.cpp" > "$stdin_scry" || true
if diff -u "$stdin_rg" "$stdin_scry" >/dev/null; then
    echo "PASS Stdin piping (< src/main.cpp)"
    PASS=$((PASS + 1))
else
    echo "FAIL Stdin piping (< src/main.cpp)"
    diff -u "$stdin_rg" "$stdin_scry" | head -n 20
    FAIL=$((FAIL + 1))
fi
rm -f "$stdin_rg" "$stdin_scry"

# Quiet mode (-q) exit code check (search src/ so this script doesn't match itself!)
if "$SCRY" -q --no-daemon "bp128_unpack_deltas" "$TARGET/src" && ! "$SCRY" -q --no-daemon "definitely_not_present_zz99" "$TARGET/src"; then
    echo "PASS Quiet mode (-q exit codes)"
    PASS=$((PASS + 1))
else
    echo "FAIL Quiet mode (-q exit codes)"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "Differential tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
