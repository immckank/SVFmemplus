#!/usr/bin/env bash
# Unified static-analysis output contract.
# Usage:
#   run_saber_all_defects.sh --output-dir DIR [--no-bof] input.bc [input2.bc ...]

set -euo pipefail

OUTPUT_DIR=""
RUN_BOF=1
BC_FILES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      [[ $# -ge 2 ]] || { echo "error: --output-dir requires a directory" >&2; exit 2; }
      OUTPUT_DIR=$2
      shift 2
      ;;
    --no-bof)
      RUN_BOF=0
      shift
      ;;
    --)
      shift
      BC_FILES+=("$@")
      break
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      exit 2
      ;;
    *)
      BC_FILES+=("$1")
      shift
      ;;
  esac
done

[[ -n "$OUTPUT_DIR" ]] || { echo "error: --output-dir is required" >&2; exit 2; }
[[ ${#BC_FILES[@]} -gt 0 ]] || { echo "error: at least one .bc input is required" >&2; exit 2; }
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR=$(cd "$OUTPUT_DIR" && pwd)

stem_of() {
  basename "$1" .bc
}

run_saber() {
  local flag=$1 tag=$2 bc=$3 stem log
  stem=$(stem_of "$bc")
  log="$OUTPUT_DIR/${stem}_${tag}.txt"
  echo "==> saber ${flag} ${bc}"
  saber "$flag" -report-dir="$OUTPUT_DIR" "$bc" 2>&1 | tee "$log"
}

run_bof() {
  local bc=$1 stem log
  stem=$(stem_of "$bc")
  log="$OUTPUT_DIR/${stem}_bof.txt"
  echo "==> bof ${bc}"
  bof "$bc" 2>&1 | tee "$log"
}

for bc in "${BC_FILES[@]}"; do
  [[ -f "$bc" ]] || { echo "error: bitcode not found: $bc" >&2; exit 1; }
  run_saber -leak leak "$bc"
  run_saber -dfree dfree "$bc"
  run_saber -uaf uaf "$bc"
  run_saber -uninit uninit "$bc"
  if [[ "$RUN_BOF" -eq 1 ]]; then
    run_bof "$bc"
  fi
done

echo "Analysis outputs: $OUTPUT_DIR"
