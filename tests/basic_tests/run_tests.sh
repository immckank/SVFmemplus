#!/usr/bin/env bash
#
# run_tests.sh -- BOF (buffer-overflow / out-of-bounds) regression runner.
#
# For every *.c case it:
#   1. compiles to LLVM IR with clang (-g -O0, value names kept),
#   2. runs the `bof` detector,
#   3. counts MUST / MAY reports and checks them against the expected baseline.
#
# Environment overrides (all auto-detected by default):
#   CLANG   path to clang   (default: tries llvm@16 then PATH)
#   BOF     path to bof bin  (default: <repo>/Release-build/bin/bof or bin/bof)
#
# Exit code 0 == all cases pass, non-zero otherwise.

set -u

# ----------------------------------------------------------------------------
# Locate tools.
# ----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# tests/basic_tests/ -> repo root (SVFmemplus)
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

if [[ -z "${CLANG:-}" ]]; then
    for c in /opt/homebrew/opt/llvm@16/bin/clang \
             /usr/local/opt/llvm@16/bin/clang \
             "$(command -v clang-16 2>/dev/null)" \
             "$(command -v clang 2>/dev/null)"; do
        if [[ -n "$c" && -x "$c" ]]; then CLANG="$c"; break; fi
    done
fi

if [[ -z "${BOF:-}" ]]; then
    for b in "$REPO_ROOT/Release-build/bin/bof" \
             "$REPO_ROOT/bin/bof" \
             "$(command -v bof 2>/dev/null)"; do
        if [[ -n "$b" && -x "$b" ]]; then BOF="$b"; break; fi
    done
fi

if [[ -z "${CLANG:-}" || ! -x "${CLANG:-}" ]]; then
    echo "ERROR: clang not found. Set CLANG=/path/to/clang." >&2
    exit 2
fi
if [[ -z "${BOF:-}" || ! -x "${BOF:-}" ]]; then
    echo "ERROR: bof binary not found. Build first (setup.sh + build.sh) or set BOF=." >&2
    exit 2
fi

echo "CLANG = $CLANG"
echo "BOF   = $BOF"
echo

# ----------------------------------------------------------------------------
# Expected baseline:  <case>  <expected MUST>  <expected MAY (-1 = >=1, ignore exact)>
# ----------------------------------------------------------------------------
declare -a CASES=(
    "stack_oob       2  -1"   # a[10], a[16] MUST; loop a[i] -> MAY
    "single_byte     1   0"   # b[1] MUST; b[0] safe
    "heap_oob        3   0"   # malloc/calloc/realloc MUST; heap_safe none
    "memcpy_oob      3   0"   # memcpy/memset/strncpy MUST; copy_safe none
    "interproc_oob   2   0"   # k=1 context: write_at(a,11) & write_at(a,10) MUST; (a,9) safe
    "complex_index   3   0"   # a[b*c] & a[b*c+b] via scalar formals: g[12],g[10],m[20] MUST
    "complex_heap    2   0"   # heap a[b*c] byte-scaled: p[16],q[9] MUST; p[9],q[6] safe
    "struct_oob      1   0"   # struct field index OOB MUST; struct_safe none
    "alloc_offbyone_memcpy_s  1  1"   # symbolic under-alloc: const_under MUST, cond_under MAY, exact safe
)

pass=0
fail=0

for entry in "${CASES[@]}"; do
    read -r name expMust expMay <<< "$entry"
    src="$SCRIPT_DIR/$name.c"
    ll="$SCRIPT_DIR/$name.ll"

    if [[ ! -f "$src" ]]; then
        echo "[SKIP] $name (source missing)"
        continue
    fi

    if ! "$CLANG" -S -emit-llvm -g -O0 -fno-discard-value-names "$src" -o "$ll" 2>/dev/null; then
        echo "[FAIL] $name (clang compile error)"
        fail=$((fail+1))
        continue
    fi

    out="$("$BOF" "$ll" 2>&1)"
    gotMust=$(printf '%s\n' "$out" | grep -c "MUST buffer overflow")
    gotMay=$(printf '%s\n' "$out" | grep -c "MAY buffer overflow")

    ok=1
    [[ "$gotMust" -ne "$expMust" ]] && ok=0
    if [[ "$expMay" -ge 0 ]]; then
        [[ "$gotMay" -ne "$expMay" ]] && ok=0
    else
        [[ "$gotMay" -lt 1 ]] && ok=0
    fi

    if [[ "$ok" -eq 1 ]]; then
        printf "[PASS] %-14s MUST=%s MAY=%s\n" "$name" "$gotMust" "$gotMay"
        pass=$((pass+1))
    else
        printf "[FAIL] %-14s MUST=%s (exp %s)  MAY=%s (exp %s)\n" \
               "$name" "$gotMust" "$expMust" "$gotMay" "$expMay"
        echo "------ detector output ------"
        printf '%s\n' "$out" | grep -A3 "buffer overflow"
        echo "-----------------------------"
        fail=$((fail+1))
    fi
done

echo
echo "Summary: $pass passed, $fail failed."
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
