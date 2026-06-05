#!/usr/bin/env bash
# 对给定 .bc 依次运行 saber（leak / dfree / uaf / uninit）与 bof，日志写入 <bc 主文件名>_<缺陷>.txt
# 用法:
#   ./run_saber_all_defects.sh [path/to/a.bc path/to/b.bc ...]
# 无参数时默认测当前目录下的:
#   ubs_engine_mem_defects_static.bc  ubs_engine_mem_defects.bc

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -gt 0 ]]; then
  BC_FILES=("$@")
else
  BC_FILES=(
    "${SCRIPT_DIR}/ubs_engine_mem_defects_static.bc"
    "${SCRIPT_DIR}/ubs_engine_mem_defects.bc"
  )
fi

stem_of() {
  basename "$1" .bc
}

run_saber() {
  local flag=$1
  local tag=$2
  local bc=$3
  local stem out
  stem=$(stem_of "$bc")
  out="${stem}_${tag}.txt"
  echo "==> saber ${flag} ${bc} -> ${out}"
  saber "${flag}" "$bc" 2>&1 | tee "$out"
}

run_bof() {
  local bc=$1
  local stem out
  stem=$(stem_of "$bc")
  out="${stem}_bof.txt"
  echo "==> bof ${bc} -> ${out}"
  bof "$bc" 2>&1 | tee "$out"
}

for bc in "${BC_FILES[@]}"; do
  [[ -f "$bc" ]] || { echo "error: 找不到位码文件: $bc" >&2; exit 1; }
  run_saber -leak leak "$bc"
  run_saber -dfree dfree "$bc"
  run_saber -uaf uaf "$bc"
  run_saber -uninit uninit "$bc"
  run_bof "$bc"
done

echo "全部完成。"
