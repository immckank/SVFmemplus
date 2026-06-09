#!/usr/bin/env bash
#
# pack_bof.sh -- package the BOF module into a single portable tarball.
#
# Bundles, with paths RELATIVE to the SVFmemplus root (so unpack_bof.sh can
# restore them in-place on another tree):
#   - tests/                  全部回归用例（basic_tests / advanced_tests 等）
#   - build_bof.sh            BOF 增量编译脚本
#   - pack_bof.sh             本压缩脚本（一并打入，便于再次分发）
#   - unpack_bof.sh           解压缩脚本（一并打入，随包携带）
#   - svf/include/BOF/        BOF 头文件
#   - svf/lib/BOF/            BOF 实现
#   - svf-llvm/tools/BOF/     bof 命令行工具入口 + CMakeLists
#
# Usage:
#   ./pack_bof.sh                 # 生成 bof_bundle.tar.gz（默认）
#   ./pack_bof.sh my_bundle.tgz   # 指定输出文件名
#
# 还原请使用同目录的 unpack_bof.sh。

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
cd "${SCRIPT_DIR}"

OUTPUT="${1:-bof_bundle.tar.gz}"

# 待打包的条目（相对 SVFmemplus 根目录的路径）。
ITEMS=(
    "tests"
    "build_bof.sh"
    "pack_bof.sh"
    "unpack_bof.sh"
    "svf/include/BOF"
    "svf/lib/BOF"
    "svf-llvm/tools/BOF"
)

# 逐项校验存在性，缺失即报错退出，避免打出不完整的包。
missing=0
for item in "${ITEMS[@]}"; do
    if [[ ! -e "${item}" ]]; then
        echo "ERROR: '${item}' 不存在于 ${SCRIPT_DIR}" >&2
        missing=1
    fi
done
[[ "${missing}" -eq 0 ]] || exit 1

echo "正在打包以下条目 -> ${OUTPUT}"
for item in "${ITEMS[@]}"; do
    echo "  + ${item}"
done

# -c 创建、-z gzip 压缩、-f 指定输出；条目均为相对路径，还原时结构原样恢复。
tar -czf "${OUTPUT}" "${ITEMS[@]}"

SIZE="$(du -h "${OUTPUT}" | cut -f1)"
echo
echo "完成。生成压缩包：${SCRIPT_DIR}/${OUTPUT}（${SIZE}）"
echo "还原命令：./unpack_bof.sh ${OUTPUT} [目标根目录]"
