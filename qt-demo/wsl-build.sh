#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-wsl-aarch64"
DEPLOY_DIR="${SCRIPT_DIR}/deploy-wsl-aarch64"
TOOLCHAIN_FILE="${SCRIPT_DIR}/wsl-toolchain.cmake"

MODE="${1:-all}"

if [[ "${MODE}" == "clean" ]]; then
    echo "=== clean build/deploy ==="
    rm -rf "${BUILD_DIR}" "${DEPLOY_DIR}"
fi

source /opt/rk3568_kernel_pack/qt6-aarch64-env.sh

mkdir -p "${BUILD_DIR}"

echo "=== configure ==="
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_COLOR_MAKEFILE=ON \
    -Wno-dev

echo "=== build ==="
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo "=== built: ${BUILD_DIR}/appqt-demo ==="
file "${BUILD_DIR}/appqt-demo" || true

if [[ -x "${SCRIPT_DIR}/../tools/update_compile_commands.sh" ]]; then
    "${SCRIPT_DIR}/../tools/update_compile_commands.sh" || true
fi

if [[ "${MODE}" == "build" ]]; then
    echo "=== build only, skip deploy ==="
    exit 0
fi

echo "=== deploy: ${DEPLOY_DIR} ==="
rm -rf "${DEPLOY_DIR}"
mkdir -p "${DEPLOY_DIR}/bin"
cp "${BUILD_DIR}/appqt-demo" "${DEPLOY_DIR}/bin/"

echo "=== done ==="
find "${DEPLOY_DIR}" -type f | sed "s|${DEPLOY_DIR}/|  |g"
