#!/bin/bash
set -e

jobs=4

#########
# 基本路径
#########
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
SVFHOME="${SCRIPT_DIR}"

LLVM_VERSION=16.0.4
LLVM_SRC_URL="https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-${LLVM_VERSION}.zip"
LLVM_HOME="llvm-${LLVM_VERSION}.obj"

Z3_VERSION=4.8.8
Z3_SRC_URL="https://github.com/Z3Prover/z3/archive/refs/tags/z3-${Z3_VERSION}.zip"
Z3_HOME="z3.obj"

BUILD_TYPE=Release
if [[ $1 =~ ^[Dd]ebug$ ]]; then
    BUILD_TYPE=Debug
fi

#########
# 工具检查
#########
function check_tool {
    if ! command -v $1 &> /dev/null; then
        echo "❌ Missing tool: $1"
        exit 1
    fi
}

check_tool cmake
check_tool unzip
check_tool make
check_tool gcc
check_tool g++

#########
# 下载函数
#########
function download {
    url=$1
    file=$2
    if [[ ! -f "$file" ]]; then
        echo "⬇️ Downloading $file ..."
        curl -L "$url" -o "$file"
    fi
}

#########
# 构建 LLVM（关键：关闭 terminfo）
#########
function build_llvm {
    if [[ -d "$LLVM_HOME" ]]; then
        echo "✅ LLVM already exists"
        return
    fi

    echo "🚀 Building LLVM from source (NO terminfo)..."

    download "$LLVM_SRC_URL" llvm.zip
    unzip -q llvm.zip
    SRC_DIR=$(find . -maxdepth 1 -type d -name "llvm-project-*")

    mkdir llvm-build
    cd llvm-build

    cmake -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SVFHOME/$LLVM_HOME" \
        -DLLVM_ENABLE_PROJECTS="clang" \
        -DLLVM_ENABLE_RTTI=ON \
        -DLLVM_BUILD_LLVM_DYLIB=ON \
        -DLLVM_LINK_LLVM_DYLIB=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DLLVM_ENABLE_TERMINFO=OFF \
        ../$SRC_DIR/llvm

    make -j${jobs}
    make install

    cd ..
    rm -rf llvm-build llvm.zip "$SRC_DIR"

    echo "✅ LLVM build done"
}

#########
# 构建 Z3
#########
function build_z3 {
    if [[ -d "$Z3_HOME" ]]; then
        echo "✅ Z3 already exists"
        return
    fi

    echo "🚀 Building Z3 from source..."

    download "$Z3_SRC_URL" z3.zip
    unzip -q z3.zip
    SRC_DIR=$(find . -maxdepth 1 -type d -name "z3-*")

    mkdir z3-build
    cd z3-build

    cmake -DCMAKE_INSTALL_PREFIX="$SVFHOME/$Z3_HOME" \
          -DZ3_BUILD_LIBZ3_SHARED=false \
          ../$SRC_DIR

    make -j${jobs}
    make install

    cd ..
    rm -rf z3-build z3.zip "$SRC_DIR"

    echo "✅ Z3 build done"
}

#########
# 主流程
#########

build_llvm
build_z3

export LLVM_DIR="$SVFHOME/$LLVM_HOME/lib/cmake/llvm"
export PATH="$SVFHOME/$LLVM_HOME/bin:$PATH"
export LD_LIBRARY_PATH="$SVFHOME/$LLVM_HOME/lib:$LD_LIBRARY_PATH"

export Z3_DIR="$SVFHOME/$Z3_HOME"

echo "LLVM_DIR=$LLVM_DIR"
echo "Z3_DIR=$Z3_DIR"

#########
# 构建 SVF
#########
BUILD_DIR="${BUILD_TYPE}-build"
rm -rf "$BUILD_DIR"
mkdir "$BUILD_DIR"

cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DSVF_ENABLE_ASSERTIONS=true \
      -DBUILD_SHARED_LIBS=OFF \
      -S "$SVFHOME" -B "$BUILD_DIR"

cmake --build "$BUILD_DIR" -j${jobs}

#########
# 环境加载
#########
source ${SVFHOME}/setup.sh ${BUILD_TYPE}

echo "🎉 Build finished successfully!"
