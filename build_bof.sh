#!/usr/bin/env bash
#
# build_bof.sh -- incremental build helper for the `bof` detector.
#
# Unlike ./build.sh (which wipes ${BUILD_TYPE}-build and does a full configure +
# rebuild every time), this script REUSES the existing CMake build tree and only
# rebuilds the `bof` target (and its dependency SvfCore, which contains the BOF
# sources under svf/lib/BOF). Intended for fast edit-compile-test loops while
# debugging the BOF module.
#
# Usage:
#   ./build_bof.sh            # incremental build of bof in Release-build
#   ./build_bof.sh debug      # incremental build of bof in Debug-build
#   JOBS=16 ./build_bof.sh    # override parallelism
#
# If the build tree does not exist yet, run ./build.sh [debug] once first.

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
SVFHOME="${SCRIPT_DIR}"

JOBS="${JOBS:-8}"

# Pick build type / directory (default Release, matching build.sh).
BUILD_TYPE='Release'
if [[ "${1:-}" =~ ^[Dd]ebug$ ]]; then
    BUILD_TYPE='Debug'
fi
BUILD_DIR="${SVFHOME}/${BUILD_TYPE}-build"

if [[ ! -d "${BUILD_DIR}" || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    echo "ERROR: build tree '${BUILD_DIR}' not found (or not configured)."
    echo "       Run a full build first:  ./build.sh ${BUILD_TYPE,,}"
    exit 1
fi

echo "Incrementally building target 'bof' in ${BUILD_DIR} (jobs=${JOBS})..."
cmake --build "${BUILD_DIR}" --target bof -j "${JOBS}"

BOF_BIN="${BUILD_DIR}/bin/bof"
echo
if [[ -x "${BOF_BIN}" ]]; then
    echo "Done. bof => ${BOF_BIN}"
else
    echo "Build finished, but ${BOF_BIN} not found -- check output above."
fi
