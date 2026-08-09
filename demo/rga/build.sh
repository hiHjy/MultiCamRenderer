#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TOOLCHAIN_FILE="${ROOT_DIR}/qt-demo/toolchain.cmake"

MODE="${1:-build}"

if [ "${MODE}" = "clean" ]; then
    rm -rf "${BUILD_DIR}" "${SCRIPT_DIR}/rga_ops_demo" "${SCRIPT_DIR}"/out_*.png
fi

mkdir -p "${BUILD_DIR}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

cp "${BUILD_DIR}/rga_ops_demo" "${SCRIPT_DIR}/rga_ops_demo"

echo "built: ${SCRIPT_DIR}/rga_ops_demo"
