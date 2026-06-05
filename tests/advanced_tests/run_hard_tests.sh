#!/usr/bin/env bash
#
# run_hard_tests.sh -- BOF "advanced" (real-world defect-injection) runner.
#
# Unlike basic_tests/run_tests.sh (which compiles tiny *.c cases and asserts an
# exact MUST/MAY baseline), this runner targets large pre-compiled LLVM bitcode
# (*.bc) extracted from kernel drivers / ubs_engine / OpenHarmony, with injected
# memory defects. The `bof` detector only handles BUFFER_OVERFLOW (BO); the other
# injected classes (MEMORY_LEAK / USE_AFTER_FREE / DOUBLE_FREE / UNINITIALIZED)
# are out of scope and intentionally ignored in the ground truth below.
#
# It does NOT fail on misses -- the goal is to *collect* detection results and
# compare against the BO ground truth so we can analyse why hard cases are
# missed. Per-case raw output goes to results/<case>.out.txt and an aggregate
# results/SUMMARY.md is generated.
#
# Environment overrides:
#   BOF   path to bof binary (default: <repo>/Release-build/bin/bof or bin/bof)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# tests/advanced_tests/ -> repo root (SVFmemplus)
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CASES_DIR="$SCRIPT_DIR/cases"
RESULTS_DIR="$SCRIPT_DIR/results"

# ----------------------------------------------------------------------------
# Locate bof.
# ----------------------------------------------------------------------------
if [[ -z "${BOF:-}" ]]; then
    for b in "$REPO_ROOT/Release-build/bin/bof" \
             "$REPO_ROOT/bin/bof" \
             "$(command -v bof 2>/dev/null)"; do
        if [[ -n "$b" && -x "$b" ]]; then BOF="$b"; break; fi
    done
fi
if [[ -z "${BOF:-}" || ! -x "${BOF:-}" ]]; then
    echo "ERROR: bof binary not found. Build first (./build.sh) or set BOF=." >&2
    exit 2
fi

echo "BOF     = $BOF"
echo "CASES   = $CASES_DIR"
echo "RESULTS = $RESULTS_DIR"
echo

mkdir -p "$RESULTS_DIR"

# ----------------------------------------------------------------------------
# Case table:  <name>  <bc relative to cases/>  <expected BO defects>  <source>
#
# Expected BO counts are the number of injected BUFFER_OVERFLOW defects taken
# from the inline /* DEFECT-N: BUFFER_OVERFLOW */ markers (50-defect .c files)
# or the defect-injection report .md (10-defect / openharmony sets).
# ----------------------------------------------------------------------------
declare -a CASES=(
    "object1_kernel_50|object1_kernel_50/sentry_remote_client_defects.bc|10|kernel sentry driver, 50 defects (BO: DEFECT-2,7,11,16,21,27,32,37,42,47)"
    "object3_ubs_50|object3_ubs_50/ubs_engine_mem_defects.with_ipc_free.bc|10|ubs_engine, 50 defects (BO: DEFECT-2,7,11,16,21,27,32,37,42,47)"
    "object1_kernel_10|object1_kernel_10/svf_subset.bc|2|kernel sentry driver subset, 10 defects (BO-1,BO-2)"
    "object3_ubs_10|object3_ubs_10/sdk_subset.bc|2|ubs_engine sdk subset, 10 defects (BO-1,BO-2)"
    "openharmony_40|openharmony_40/openharmony_wlan_client_linked.bc|8|OpenHarmony wlan client, 40 defects (BO-1..BO-8)"
)

SUMMARY="$RESULTS_DIR/SUMMARY.md"
{
    echo "# BOF 困难版测试结果汇总"
    echo
    echo "- 生成时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "- 检测器: \`$BOF\`"
    echo "- 说明: \`bof\` 仅检测 BUFFER_OVERFLOW(BO) 类缺陷；ML/UAF/DF/UU 不在职责范围，不计入 ground truth。"
    echo "- 本表统计 **检测到的 MUST/MAY 报告数** 与 **注入的 BO 缺陷数(期望)** 的对照；困难版以收集+分析为目标，不因漏报判失败。"
    echo
    echo "| 用例 | 期望BO | MUST | MAY | 检测合计 | 覆盖率(检测/期望) | 来源 |"
    echo "| --- | ---: | ---: | ---: | ---: | ---: | --- |"
} > "$SUMMARY"

total_expected=0
total_detected=0

for entry in "${CASES[@]}"; do
    IFS='|' read -r name bc expBO src <<< "$entry"
    bcpath="$CASES_DIR/$bc"
    out="$RESULTS_DIR/$name.out.txt"

    if [[ ! -f "$bcpath" ]]; then
        echo "[SKIP] $name (bitcode missing: $bc)"
        echo "| $name | $expBO | - | - | - | SKIP(缺bc) | $src |" >> "$SUMMARY"
        continue
    fi

    echo "[RUN ] $name  ($bc)"
    "$BOF" "$bcpath" > "$out" 2>&1
    rc=$?

    # grep -c always prints a count (0 when no match); its exit status 1 on
    # "no match" is harmless here and must NOT trigger a fallback echo (which
    # would corrupt the value into a multi-line string).
    must=$(grep -c "MUST buffer overflow" "$out" 2>/dev/null) || true
    may=$(grep -c "MAY buffer overflow" "$out" 2>/dev/null) || true
    detected=$((must + may))

    # crash / no-report diagnostics appended to the per-case log tail
    if [[ $rc -ne 0 ]]; then
        echo "    (warning: bof exit code $rc -- see $name.out.txt)"
    fi

    cov="0%"
    if [[ "$expBO" -gt 0 ]]; then
        cov="$(awk "BEGIN{printf \"%.0f%%\", ($detected/$expBO)*100}")"
    fi

    printf "       expected BO=%s  MUST=%s  MAY=%s  detected=%s  coverage=%s\n" \
           "$expBO" "$must" "$may" "$detected" "$cov"

    echo "| $name | $expBO | $must | $may | $detected | $cov | $src |" >> "$SUMMARY"

    total_expected=$((total_expected + expBO))
    total_detected=$((total_detected + detected))
done

{
    echo "| **合计** | **$total_expected** | | | **$total_detected** | | |"
    echo
    echo "## 各用例检测到的越界位置（line/file）"
    echo
    for entry in "${CASES[@]}"; do
        IFS='|' read -r name bc expBO src <<< "$entry"
        out="$RESULTS_DIR/$name.out.txt"
        [[ -f "$out" ]] || continue
        echo "### $name"
        echo
        echo '```'
        grep -E "buffer overflow|Location" "$out" | head -80
        echo '```'
        echo
    done
} >> "$SUMMARY"

echo
echo "Done. 逐用例原始输出见 $RESULTS_DIR/<case>.out.txt"
echo "汇总报告: $SUMMARY"
