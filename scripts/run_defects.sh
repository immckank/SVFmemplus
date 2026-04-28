#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
用法:
  ./run_defects.sh <输入.bc或目录> <输出目录>

说明:
  - 输入可为单个 .bc 文件或包含 .bc 的目录（递归扫描）。
  - 运行缺陷类型: leak, dfree, uninit, uaf, bof
  - 输出路径:
      <输出目录>/leak/<name>.txt
      <输出目录>/dfree/<name>.txt
      <输出目录>/uninit/<name>.txt
      <输出目录>/uaf/<name>.txt
      <输出目录>/bof/<name>.txt
  - 同时在终端显示与写文件（stdout+stderr）。
EOF
}

if [[ $# -ne 2 ]]; then
  usage
  exit 1
fi

input_path="$1"
output_root="$2"

if [[ ! -e "$input_path" ]]; then
  echo "错误: 输入路径不存在: $input_path" >&2
  exit 1
fi

mkdir -p "$output_root"/{leak,dfree,uninit,uaf,bof}

run_one_bc() {
  local bc_file="$1"
  local base_name
  local leak_out
  local dfree_out
  local uninit_out
  local uaf_out
  local bof_out

  base_name="$(basename "${bc_file%.bc}")"
  leak_out="$output_root/leak/${base_name}.txt"
  dfree_out="$output_root/dfree/${base_name}.txt"
  uninit_out="$output_root/uninit/${base_name}.txt"
  uaf_out="$output_root/uaf/${base_name}.txt"
  bof_out="$output_root/bof/${base_name}.txt"

  echo "=== 分析: $bc_file ==="
  echo "[leak]   -> $leak_out"
  saber -leak "$bc_file" |& tee "$leak_out"

  echo "[dfree]  -> $dfree_out"
  saber -dfree "$bc_file" |& tee "$dfree_out"

  echo "[uninit] -> $uninit_out"
  saber -uninit "$bc_file" |& tee "$uninit_out"

  echo "[uaf]    -> $uaf_out"
  saber -uaf "$bc_file" |& tee "$uaf_out"

  echo "[bof]    -> $bof_out"
  bof "$bc_file" |& tee "$bof_out"
}

if [[ -f "$input_path" ]]; then
  if [[ "$input_path" != *.bc ]]; then
    echo "错误: 输入文件不是 .bc: $input_path" >&2
    exit 1
  fi
  run_one_bc "$input_path"
elif [[ -d "$input_path" ]]; then
  mapfile -t bc_files < <(rg --files -g '*.bc' "$input_path" | sort)
  if [[ ${#bc_files[@]} -eq 0 ]]; then
    echo "错误: 目录下未找到 .bc 文件: $input_path" >&2
    exit 1
  fi

  for bc in "${bc_files[@]}"; do
    run_one_bc "$bc"
  done
else
  echo "错误: 输入路径既不是文件也不是目录: $input_path" >&2
  exit 1
fi

echo "全部完成，结果位于: $output_root"
