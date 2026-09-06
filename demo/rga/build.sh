#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-wsl-aarch64"
TOOLCHAIN_FILE="${ROOT_DIR}/qt-demo/wsl-toolchain.cmake"

MODE="${1:-build}"

if [ "${MODE}" = "clean" ]; then
    rm -rf "${BUILD_DIR}" "${SCRIPT_DIR}/rga_ops_demo" "${SCRIPT_DIR}"/out_*.png
fi

source /opt/rk3568_kernel_pack/qt6-aarch64-env.sh

mkdir -p "${BUILD_DIR}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

cp "${BUILD_DIR}/rga_ops_demo" "${SCRIPT_DIR}/rga_ops_demo"

echo "built: ${SCRIPT_DIR}/rga_ops_demo"
