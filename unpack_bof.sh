#!/usr/bin/env bash
#
# unpack_bof.sh -- restore (OVERWRITE) the BOF module from a tarball created by
# pack_bof.sh.
#
# 还原以下条目（覆盖式：先删除目标已存在的同名条目，再从压缩包解出，
# 保证不残留旧文件）：
#   - tests/
#   - build_bof.sh
#   - pack_bof.sh
#   - unpack_bof.sh
#   - svf/include/BOF/
#   - svf/lib/BOF/
#   - svf-llvm/tools/BOF/
#
# Usage:
#   ./unpack_bof.sh                       # 从 bof_bundle.tar.gz 还原到脚本所在目录
#   ./unpack_bof.sh my_bundle.tgz         # 指定压缩包
#   ./unpack_bof.sh my_bundle.tgz /path   # 指定压缩包 + 还原到 /path（SVFmemplus 根）

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"

ARCHIVE="${1:-bof_bundle.tar.gz}"
DEST="${2:-${SCRIPT_DIR}}"

if [[ ! -f "${ARCHIVE}" ]]; then
    echo "ERROR: 压缩包 '${ARCHIVE}' 不存在" >&2
    exit 1
fi
# 规整为绝对路径（tar -C 需要，且后续 rm 更安全）。
ARCHIVE="$(cd -- "$(dirname -- "${ARCHIVE}")" &> /dev/null && pwd)/$(basename -- "${ARCHIVE}")"

mkdir -p "${DEST}"
DEST="$(cd -- "${DEST}" &> /dev/null && pwd)"

# 与 pack_bof.sh 保持一致的条目列表（相对 SVFmemplus 根）。
ITEMS=(
    "tests"
    "build_bof.sh"
    "pack_bof.sh"
    "unpack_bof.sh"
    "svf/include/BOF"
    "svf/lib/BOF"
    "svf-llvm/tools/BOF"
)

echo "还原目标根目录：${DEST}"
echo "使用压缩包：${ARCHIVE}"

# 覆盖式：先清除目标中已存在的同名条目，避免旧文件残留。
for item in "${ITEMS[@]}"; do
    target="${DEST}/${item}"
    if [[ -e "${target}" ]]; then
        echo "  覆盖：删除旧的 ${item}"
        rm -rf "${target}"
    fi
done

# -x 解压、-z gzip、-f 指定包、-C 指定解压根目录；相对路径结构原样恢复。
tar -xzf "${ARCHIVE}" -C "${DEST}"

# 还原后保持脚本可执行权限。
for s in build_bof.sh pack_bof.sh unpack_bof.sh; do
    [[ -f "${DEST}/${s}" ]] && chmod +x "${DEST}/${s}"
done

echo
echo "完成。已还原以下条目到 ${DEST}："
for item in "${ITEMS[@]}"; do
    echo "  - ${item}"
done
